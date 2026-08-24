# openhanko-firmware

Firmware for [OpenHanko](https://openhanko.io): a USB device that authenticates
macOS login, `sudo` and authorisation prompts. It enumerates as a PIV smart card
over USB CCID and signs with a P-256 key it generated itself, only while someone
is touching it.

```
$ system_profiler SPSmartCardsDataType
    Readers:
      #01: OpenHanko Smart Card (ATR:{length = 4, bytes = 0x3b800101})
```

Derived from [ZimengXiong/tinyTouch](https://github.com/ZimengXiong/tinyTouch)
(`firmware/tiny_touch_smartcard`); the PIV applet and CCID transport are
substantially upstream's work. See [compared with tinyTouch](#compared-with-tinytouch).

## Status

Working end to end on hardware. macOS enumerates the device, attaches a PIV
token driver, reads the certificate, pairs it to the account, and authenticates
`sudo` and the login window. The device generates its own key at first boot,
names itself after that key, and can erase itself back to factory state without
a host.

The sensor, the enrolment gesture, module binding, encryption at rest, secure
boot and debug lockout are all working on hardware. What is left:

- `CONFIG_UNLOCK` still opens on a button press rather than a fingerprint. What
  it now gates is `BOOTLOADER` and nothing else — see
  [the button does not authenticate](#the-button-does-not-authenticate).
- There is no real PIN, so a stolen device is worth what its credentials are
  worth. [THREAT-MODEL.md](THREAT-MODEL.md) says how far that goes.
- The custom RP2354A boards are at fab; development is on an RP2350-Zero.

## Compared with tinyTouch

This project exists because tinyTouch did the hard part first: a working PIV
applet and CCID transport on a microcontroller, with command chaining, `GET
RESPONSE` chunking and a correct class descriptor. That layer is still
substantially upstream's, and the differences below are what happened when the
same idea was pointed at a different problem — a device you hand to somebody
else, rather than one you build for yourself.

Measured against `firmware/tiny_touch_smartcard` as of August 2026.

| | tinyTouch | OpenHanko |
| --- | --- | --- |
| MCU | ESP32-S3 | RP2354A — RP2350 with in-package flash |
| signing algorithms | RSA-2048 only — the sign path returns `6f00` for any key that is not RSA, and `GENERAL AUTHENTICATE` rejects any `P1` but `0x07` | P-256 (`0x11`) and RSA-2048 (`0x07`), checked against the key actually loaded |
| signature time | — | 196 ms P-256 on an RP2350 |
| ECDSA nonce | n/a | RFC 6979 deterministic |
| where the key comes from | provisioned from a host, or compiled into the image | **generated on the device at first boot**; both host paths kept as alternatives |
| does the private key exist off-device | yes — on whatever machine generated it | no, on the generate-on-device path |
| slot 9D key agreement | not implemented | ECDH P-256, dynamic auth tag `0x85` |
| pinpad PIN entry | not implemented | `bPINSupport` declared and `PC_to_RDR_Secure` answered from a fingerprint |
| AIDs answered | standard PIV only | standard PIV **and** a private AID |
| driver detection | none | private-AID probe: the device works out for itself whether its driver is installed, and switches mode both ways |
| host software | Python LaunchAgent, AES/HMAC over HID | signed and notarised CryptoTokenKit extension |
| factory reset without a host | no | button held through power-up, release commits |
| on-device diagnostics | `STATUS` | `STATUS`, plus a `TRACE` ring buffer of CCID and APDU activity |
| fingerprint sensor | ZW101 | ZW111, bound to the device by its per-die serial |
| key material at rest | plaintext in NVS | AES-256-GCM under a secret in OTP |
| secure boot | none | signed images, two keys with revocation |

### What upstream has that this does not

- **A real password channel.** `touch_pin_hid.c` is 248 lines of AES/HMAC keyed
  to the host helper, so the device can type an actual password rather than a
  fixed dummy PIN. Dropped here entirely — the CryptoTokenKit route made it
  unnecessary for everything except `sudo`.
- **Dual device modes.** `device_config.c` lets one device be a PIV card *or* an
  HID password typer, selectable at runtime. This is PIV only.

### Where the differences actually matter

**Key custody.** Upstream's key is made on a host and pushed to the device, so
whoever provisioned it could have kept a copy. That is fine when you provision
your own. It stops being fine the moment somebody else assembles the device, and
no amount of assurance fixes it — the question simply should not be askable.
Generating on the device removes it: the private key has no representation
outside the chip at any point. That single change is most of why the rest of this
list exists.

**Driverless and pinpad from one image.** Upstream answers the standard PIV AID,
so Apple's `pivtoken` binds and the device types a PIN over HID into whatever has
focus. That works with nothing installed, which is genuinely the right default.
But it caps the experience there: HID typing cannot be better than the focused
window, and macOS frequently does not focus its own authorization dialog. Adding
a private AID as a *probe* gets both — untouched Mac, `pivtoken` binds, PIN typed;
driver present, our extension binds and a finger alone signs with no dialog at
all. Neither mode needs the user to choose.

**P-256 was forced, then turned out to matter.** The ESP32-S3 has a big-integer
accelerator, so upstream never paid for RSA-2048. The Cortex-M0+ has no 64-bit
multiply, and the same signature costs 2924 ms — long enough that the device
reads as broken. P-256 brings it to 469 ms. The security consequence came after:
a weak RNG is survivable for RSA blinding and fatal for an ECDSA nonce, which is
why signing here is RFC 6979 deterministic and why the RP2350's TRNG is the
reason to move parts at all.

None of this is a criticism of upstream, which is explicit about being a proof of
concept and is a good one.

## Hardware

**RP2354A** — an RP2350 die with 2 MB of flash stacked in the same package —
with an HLK-ZW111 fingerprint module. Development is on an RP2350-Zero with the
sensor wired to the same pins.

Three properties make this the part rather than a faster one. It verifies a
signed image at boot; it can permanently disable SWD through OTP; and
`get_rand_64()` is backed by a hardware TRNG, which is what makes a generated key
worth anything — a key is only as good as the randomness behind it, and one
generated without a true entropy source looks like a key and is not.

The stacked flash matters too. On a part with an external flash chip, lifting it
off with hot air and reading it is a ten-minute job needing no skill. In-package,
that becomes decapsulation and probing — still possible, no longer casual.

**Buy A4 stepping.** Errata E16, E20 and E24 are fixed in silicon and in no other
way; on A2 a glitched chip can re-enable debug and read OTP, boot unsigned code,
or have secure boot defeated with a laser. `STATUS` reports what you actually
have as `chip=`.

### Why not the RP2040

It was the development platform through most of this project and is no longer
supported. Its debug port cannot be closed, so anyone who opens the case reads
the key out and steps around the sensor entirely — which makes the fingerprint
decorative rather than protective. And `get_rand_64()` there is ring-oscillator
jitter, so keys generated on one are development keys whatever else is done. It
was also 2.4× slower at the only operation that matters: 469 ms to sign against
196 ms here.

### Pinout

| function | pin |
| --- | --- |
| fingerprint module | UART1 @57600: **GP4** = MCU TX, **GP5** = MCU RX, **GP6** TouchOut |
| configuration button | **GP12** to GND, internal pull-up — factory reset and enrollment only, never authentication |
| indicator | none on the board: the module's own ring is the entire indicator |

There is no discrete LED. A production unit is a sealed case, so anything on the
PCB would be invisible; `STATUS_LED_GPIO` is `-1` and `status_led.c` compiles to
no-ops. Every indication goes to the module's ring, including the factory reset
gesture — which had driven only the board LED, and would otherwise have run an
irreversible operation with no feedback at all.

The ZW111 harness is six wires, not four:

| ZW111 pin | to | direction |
| --- | --- | --- |
| 1 `V_Touch` | 3V3 | **permanently powered** — runs finger detection while the rest of the module sleeps |
| 2 `TouchOut` | GP6 | module → MCU, asserts while a finger is on the sensor |
| 3 `VCC` | 3V3 | |
| 4 `TX` | **GP5** | module → MCU |
| 5 `RX` | **GP4** | MCU → module |
| 6 `GND` | GND | |

**The UART lines cross.** The module's `TX` is an output and goes to GP5, which
is the MCU's receiver; the module's `RX` is an input and comes from GP4, the
MCU's transmitter. `FINGERPRINT_UART_TX` in `board_config.h` names the *MCU's*
TX pin, not the module's — TX to TX is two outputs driving each other, and the
symptom is a module that handshakes with nothing and looks dead.

`V_Touch` is a power rail, not an option: it runs finger detection while the
rest of the module sleeps, so it is in the harness regardless. `TouchOut` is the
only wire that could have been left out, and is not.

It earns the pin twice. Finger detection becomes a GPIO read instead of a
`PS_GetImage` round trip, so an idle device stops holding a conversation with
the module several times a second — and a match is refused unless the line
agrees that something is touching the sensor, at both the start and the end of
the capture. That turns forging a match from "replay bytes on RX" into "drive
two lines in a plausible time relationship". Cost, not authentication — see
[the sensor link](#the-sensor-link-cannot-be-authenticated).

**It is active-high**, which the datasheet does not say — it names the pin,
calls it a wake IRQ, and leaves the level to the unpublished protocol note.
Established by reading `STATUS` with and without a finger on the sensor, and
since then by every authentication the device has performed: with
`FINGERPRINT_REQUIRE_TOUCH 1` a match is discarded unless the line agrees at both
ends of the capture, so a wrong polarity would mean nothing ever authenticates.
That is also the failure mode if the wire breaks — closed, not open, helped by
the pin being pulled to the inactive level so a cut or unwired TouchOut reads as
"no finger" rather than floating.

`STATUS` reports it as `touch=up|down|unwired`, which is where to look first if a
board stops accepting fingers. To rule the correlation out while debugging
something else, set `FINGERPRINT_REQUIRE_TOUCH 0`.

### Which stepping am I holding?

`STATUS` reports it as `chip=`, e.g. `chip=rp2350-a4`.

Read from the boot ROM version byte at `0x00000013`, not from
`CHIP_ID.REVISION` — **A4 changed only the boot ROM and reports the same
revision as A3**, so the revision field cannot tell them apart. `picotool` reads
the same byte. `2` is A2, `3` is A3, `4` is A4.

This is a security property, not trivia. Three findings from the RP2350 Hacking
Challenge are fixed in silicon and in no other way:

| | E16 glitch to debug + OTP | E20 unsigned boot | E24 laser fault | E9 GPIO |
| --- | --- | --- | --- | --- |
| **A2** | open | flag only | open | open |
| **A3** | fixed | flag only | open | fixed |
| **A4** | fixed | fixed | fixed | fixed |

On A2, an attacker with the device and glitching equipment can recover the
signing key whatever the firmware does. Mass production moved to A4 in July 2025;
A4 parts are marked `RP2350A0A4`.

`BOOT_FLAGS0.DISABLE_WATCHDOG_SCRATCH` is the documented mitigation for E20, and
should not be set on A3 or A4: E20 is already fixed there, so it buys nothing,
and it disables a register the SDK's application-level double-tap depends on.

### USB identity

| | |
| --- | --- |
| VID | `0x16D0` — MCS Electronics |
| PID | `0x1551` |
| serial | `openhanko.io:<12 hex>`, from the board's unique id at runtime |
| strings | manufacturer `OpenHanko`, product `Smart Card` |

The VID/PID is a sublicensed allocation, which is enough for the OS to tell the
device apart from everything else on the bus. It does not permit USB-IF logo
certification; that needs a VID of your own.

macOS builds the reader name by concatenating the manufacturer and product
strings, so they must differ or the pairing dialog reads "OpenHanko OpenHanko".

Defaults live in [`src/board_config.h`](src/board_config.h). For a bare
board with nothing wired, set `BUTTON_USE_BOOTSEL 1` to borrow the BOOTSEL
button. BOOTSEL sits on the QSPI bus rather than the GPIO bank, so reading it
overrides the flash chip-select and blacks out interrupts for tens of
microseconds; sampling is rate-limited to 20 ms to keep that off USB's back.
Bench use only.

## Build and flash

```sh
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0
cd src && cmake -S . -B build && cmake --build build
```

Hold BOOTSEL while replugging, then copy the image onto the volume that appears:

```sh
cp -X build/openhanko.uf2 /Volumes/RP2350/
```

**Double-tap RESET** enters the bootloader without holding BOOTSEL — but only on
a board where `BOOT_FLAGS1.DOUBLE_TAP` has been burned, which `bootkeys.py` does
during stage 1. On a board with nothing burned, use the BOOTSEL button or short
QSPI_SS to ground through about 1 kΩ.

That flag is the bootrom's own mechanism, not the SDK's, and the difference
matters. `pico_bootsel_via_double_reset` ran in the application: it stashed a
magic in the watchdog scratch for a second reset to catch, which needs the
firmware to boot — not the situation anyone wants a recovery path for. Enabling
secure boot stops it working entirely. The bootrom's version runs before any
application, so it rescues a board whose firmware is broken, unsigned or
missing, and it survives secure boot. It costs 200 ms of boot rather than 800.

A fresh clone needs `PICO_SDK_PATH` set explicitly. An existing `build/`
directory caches it, so a missing SDK only surfaces on a clean checkout.

## The button does not authenticate

A fingerprint match is the only thing that authorises a signature over PIV. The
button's jobs are configuration: the factory reset gesture, opening enrolment,
and unlocking the configuration console.

There is no build in which the first part is untrue. There was briefly a flag for
bench boards with no sensor fitted, and it is gone — a switch whose only function
is to restore press-to-authenticate is a switch someone eventually ships.
Removing the sensor from a unit now yields a device that cannot authenticate at
all, which is the correct failure.

`PAIRING_MODE` used to be the exception — a button press bought 120 seconds of
signing on both 9A and 9D, without even going through `CONFIG_UNLOCK`. It is
gone, along with every other console command that could reach a key or a
template. Nothing on the console signs, enrols, erases or provisions any more,
so the fingerprint is now the only thing that authorises a signature by any
route.

`CONFIG_UNLOCK` still asks for a press rather than a match. What it gates is
`BOOTLOADER`, which reboots into the ROM bootloader — on a locked unit that
still only accepts signed firmware, so the press buys an update path, not a
key.

## Enrolling a finger

Adding a finger is the one operation that keeps the key and adds a way to use
it, so it is the one that has to be authorised. Wiping cannot be abused — it
destroys the key — but adding a finger to a working device would hand someone a
credential that answers to them.

**Rest an already-enrolled finger on the sensor and click the button.** The click
marks intent; the finger authorises it. Then lift, and present the new finger.

| | ring |
| --- | --- |
| click, finger matches | two green flashes, then breathing purple |
| click, finger does not match | one long red flash, nothing opens |
| click, no finger | nothing — the click alone does not authenticate |
| waiting for the new finger, 30 s | breathing purple |
| enrolled | steady green |
| timed out or failed | steady red, nothing stored |

Two flashes against one carries the accept/reject distinction, with colour only
reinforcing it: green against red is the pair red-green colourblindness
collapses, and at the gate that flash is the only feedback there is.

**The first finger is a special case**, because there is nothing yet to match
against. A device with an identity but no template cannot authenticate for
anybody, so it opens enrollment by itself at boot and keeps offering until one
takes. That also removes the window the gesture cannot cover: there is never a
period in which the device is paired and useful but unenrolled, which is the only
period in which appropriating it would be worth anything.

**One finger is enough to get going.** A device asks for one at first boot and
stops asking once it has it. If the enrolled finger ever becomes unavailable,
factory reset is the way back: hold the button through power-up, enrol again,
pair again — a minute, and a complete recovery for unlocking a Mac.

That recovery destroys the key, though, so it is only cheap if nothing expensive
was bound to it. An SSH identity trusted by a fleet of hosts, or a CA-issued
certificate, has to be re-provisioned everywhere. Enrol a second finger through
the gate if the device is used that way.

## Identity

A device with no identity generates a P-256 keypair and self-signed certificate
at first boot. The private key is never transmitted and never copied to a host —
though it is not *unextractable*: on every state shipped so far it can be read
out of flash by someone holding the device. See
[THREAT-MODEL.md](THREAT-MODEL.md).

Certificates are named after the tail of the public key's SHA-1, which is also
how macOS derives `kSecAttrApplicationLabel` — the hash `sc_auth` pairs against:

```
$ STATUS
… source=flash alg=p256 keyrc=-0x0000 name="OpenHanko #FA764A"

$ sc_auth identities
CD01D8E61DB4E82E005F560781284FCE79FA764A   vden - Certificate For PIV Authentication (OpenHanko #FA764A)
                                  ^^^^^^
```

So a device states its own name over the console, and that name identifies its
keychain entry. Two devices on one Mac are told apart without unplugging either.

`get_rand_64()` is backed by the RP2350's hardware TRNG, which is the reason a
key generated here is worth trusting and one generated on a part without a true
entropy source is not.

### There is no other way to load one

There were two: a staged upload over the console, and `secrets.h`, which baked
PEMs into the firmware image. Both are gone, along with `PROVISION_BEGIN` /
`PROVISION_CHUNK` / `PROVISION_COMMIT` and the compiled-key path in `piv.c`.

They were bench conveniences, and each of them made "the private key never
existed off this chip" a claim about a *configuration* rather than about the
device. Removing them makes it unconditional: `STATUS` reports `source=flash`
or `source=none`, and there is no third answer.

`./provision.py` is still stdlib-only — no pip install, no venv — and now only
inspects and pairs.

### Factory reset

Hold the button through power-up. There is no console or USB command for this
and deliberately so — erasing credentials is the one operation a host should not
be able to start. It destroys the key and the enrolled templates, restores every
setting to its default, and reboots, generating a fresh identity on the way up.

The indicator blinks, faster as the deadline approaches, and goes solid at six
seconds; **releasing** is what commits. Let go early, or keep holding, and
nothing happens — once solid, unplugging while still holding is the way out.

Release is the commit deliberately. A device wedged against something in a bag
can hold a button indefinitely but cannot let go.

Measured: `OpenHanko #539755` → gesture → `OpenHanko #FA764A`, `source=flash`,
`aid=standard`.

## Pair with macOS

```sh
./provision.py ports        # find the device
./provision.py status       # what does it know?
./provision.py pair         # link it to your macOS account
sudo -k && sudo -v          # test: touch the sensor when macOS asks for the PIN
```

macOS usually offers to pair on its own when the card is inserted; the "Unpaired
SmartCard inserted" notification does the same thing as `pair`. `pair` reads the
identity hash out of `sc_auth identities` and prints the `sudo sc_auth pair`
command to run, or runs it with `--run`.

`./provision.py monitor` prints device events live, which is the fastest way to
tell whether a touch registered.

## Console commands

CDC console, `115200`. `./provision.py console '<CMD>'` sends one.

| command | effect |
| --- | --- |
| `PING` | → `PONG` |
| `STATUS` | silicon stepping, key source, algorithm, AID mode, sensor, name |
| `TRACE` / `TRACE_CLEAR` | ring buffer of CCID and APDU activity |
| `FINGERPRINT_PROBE` | re-run the link probe and report what answered |
| `FINGERPRINT_INFO` | model, firmware, manufacturer, sensor name |
| `FINGERPRINT_SN` | the module's per-die serial — what binding is against |
| `FINGERPRINT_INFO_RAW` | the raw 512-byte info page as hex, for checking the field offsets |
| `OTP_STATUS` | whether the device holds a secret, and which one, by hash |
| `AID_MODE standard\|pinpad` | force the AID mode instead of letting the probe decide |
| `BOOTLOADER` | reboot to USB mass-storage bootloader |
| `REBOOT`, `USB_RECONNECT` | as named |
| `CONFIG_UNLOCK` | required before `BOOTLOADER` |

**Nothing here is irreversible, and nothing here reaches a key.** No command
loads one, generates one, signs with one, enrols a finger, erases a template or
burns a fuse — those either moved to a physical gesture or stopped existing.
`AID_MODE` is the only one that writes anything at all, it costs a button press,
and what it writes is which AID to answer.

That matters because the console is on the same cable as the card: whatever it
can do, a host that owns the Mac can do.

`TRACE` is the most useful debugging tool here; it is the witness that settled
most of the behaviour documented below.

## AID modes

One image serves both a Mac with our CryptoTokenKit driver and a Mac without
one. The device switches on its own, in both directions, and persists the choice
across reboots.

```
standard mode        answers the PIV AID  +  the private AID as a probe
  (factory default)          │
  Apple's pivtoken binds     │  our driver SELECTs the private AID —
  a touch types the PIN      │  nothing else knows to ask for it
                             ▼
                   persist pinpad, reboot
                             │
pinpad mode                  ▼
  answers the private     our driver binds exclusively;
  AID only                a touch alone signs, LED breathing
                             │
                             │  no private-AID select within 10 s
                             │  of the host first speaking
                             ▼
                   persist standard, reboot
```

The probe is the mechanism: only our driver knows to ask for the private AID, so
a SELECT of it proves the driver is installed. The device is never told.

All four transitions measured on hardware:

| behaviour | evidence |
| --- | --- |
| driverless by default | `aid=standard`, `pivtoken` binds, touch→signature with nothing installed |
| upgrades on meeting its driver | forced to standard; back to `aid=pinpad claimed=yes` in under four seconds, unattended |
| pinpad authenticates | `sudo` on a touch, no PIN typed |
| reverts when the driver is gone | `aid=pinpad claimed=yes` → nothing registered → `aid=standard claimed=no` |

**Answering both AIDs is safe only because the state is transient.** macOS binds
exactly one token driver per card, at insertion. A card that *stays* in that
state leaves two drivers racing: we measured that race flipping between reboots,
and saw `sc_auth` file one identity under "not used for authentication" while
that card silently stopped authenticating. Here the state lasts a few hundred
milliseconds before the reboot makes it exclusive.

**Consequence:** with the driver installed, forcing standard mode does not
stick — the device meets the driver again and upgrades straight back. Driverless
behaviour on a Mac that has the driver means uninstalling it.

### Pinpad mode

The driver selects the private AID, attempts a signature and is refused with
`6982`; that refusal is what makes CryptoTokenKit call `beginAuth`, which sends
`PC_to_RDR_Secure`. The card holds the request open until a finger matches, then
signs.

There is no `ins=20` VERIFY anywhere in it: no PIN is typed or transmitted. The
`CCID 69` is the secure-PIN request, and only our driver sends one.

### Standard mode

The order inverts. Nothing asks the card anything until a PIN has been
submitted, so the match comes first, the device types six digits, `pivtoken`
sends them as `VERIFY`, and the signature follows. macOS labels the prompt
*"Certificate For PIV Authentication (…)"*, which is `pivtoken`'s own format.

The PIN is six random digits, generated fresh for each prompt, and is not a
secret: `VERIFY` discards the bytes and accepts anything. The presence check is
the only gate.

Random rather than fixed because a fixed PIN teaches a number that looks like it
means something. It does not — typing it by hand authenticates nothing, since the
signature that follows still waits on a finger. The practical corollary is worth
knowing: if you ever face a PIN box the device did not fill in, **any six digits
will do**.

## Indicator

| mode | behaviour |
| --- | --- |
| pinpad | **breathes** while waiting — macOS shows no prompt, so this is the entire invitation |
| standard | **solid flash**, 700 ms, on a match, held through the signature |

The asymmetry is inherent. **macOS says nothing to the card until a PIN has
already been submitted** — measured three times, including one where a prompt sat
on screen for ten seconds and the card's whole record of it was a power-on and a
power-off 7.5 seconds apart, with nothing asked in between.

So in standard mode there is no event to light up on; the device can only
acknowledge a match after the fact. A sensor that reads a finger without a
flicker looks broken, especially when the PIN it typed lands in a window the
user is not looking at.

A WS2812 latches its last value, so a board left glaring by earlier firmware
stays that way until something clears it — hence the boot-time clear.

## Implementation notes

### Use P-256, not RSA-2048

| signing | measured |
| --- | ---: |
| P-256 on RP2350 | **196 ms** |
| P-256 on RP2040, for comparison | 469 ms |
| RSA-2048 on RP2040 | 2924 ms |

| end to end | |
| --- | ---: |
| touch to authenticated, pinpad | ~210 ms |
| APDU transfers for the challenge | 1 (2 chained for RSA) |

RSA is slow because there is no big-integer accelerator. Signing also runs
synchronously inside the CCID transfer callback, so USB stalls for the whole
operation — tolerable at 200 ms, not at three seconds.

macOS picks the algorithm from the certificate with no configuration: a P-256
cert gets `GENERAL AUTHENTICATE` with `P1 = 0x11` instead of `0x07`.

### ECDSA is RFC 6979 deterministic

`MBEDTLS_ECDSA_DETERMINISTIC`, and not optional. A predictable ECDSA nonce
recovers the private key from a single signature, and taking the RNG out of the
signing path removes that whole class of failure rather than mitigating it. Deriving the nonce from the key and message takes the RNG out of the
signing path entirely.

### EC keys must use named-curve encoding

`MBEDTLS_PK_PARSE_EC_EXTENDED` is deliberately not enabled, so mbedTLS accepts
only named-curve EC keys and rejects explicit parameters — field type, prime,
generator, the lot — with `MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE` (`-0x4E80`).

That costs nothing now: `mbedtls_pk_write_key_pem()` in `identity.c` writes
named-curve, which is also what real PIV cards carry and is 135 bytes against
377. It cost a day when keys still came from the host, because LibreSSL's
`openssl req -newkey ec` writes explicit parameters by default and the failure
mode is confusing — the certificate decodes, the card enumerates, macOS reads
the identity, and only signing fails. `STATUS` reports `alg=` and `keyrc=`, so
if it ever recurs it is one command away from a diagnosis.

### Omitting `MBEDTLS_PEM_PARSE_C` fails the same way

Certificates still work, because `decode_pem_cert()` in `piv.c` does its own
base64. The card enumerates, macOS offers to pair, and only signing fails — with
`6f00`. `TRACE` pinned it by showing `6f00` where a missing presence check
would have given `6982`.

### Apple's `pivtoken` ignores pinpad entirely

CCID readers can declare `bPINSupport` and collect the PIN themselves, which
would mean no text field, no keystrokes, and nothing to focus. CryptoTokenKit
does read that byte out of the CCID descriptor.

With Apple's built-in `pivtoken`: **not a single `PC_to_RDR_Secure`**, even with
`bPINSupport = 0x01` declared.

With the driver in [openhanko-macos](https://github.com/openhanko/openhanko-macos)
it works — no dialog, nothing typed, authenticated by the fingerprint alone.

`sudo` is seamless, including the prompt: that repository's `tools/pam` module
runs ahead of `pam_smartcard.so` and goes through CryptoTokenKit, so the token
driver performs the authentication and the touch is the whole interaction. A
`sudo` traced on this device shows `CCID 69 Secure`, the match, and the
signature, with **no `VERIFY` at all** — nothing was typed.

What is *not* solved is applications that put up their own PIN field. Chrome's
password manager unlocks correctly but still shows a modal and takes the typed
its six random digits; pinpad governs the CryptoTokenKit-to-card leg, and an
application that collects a PIN itself never reaches it.

### Testing caveat

Smart-card token keys are only visible from a process with a full user session.
Automated shells — CI, agents, some launchd contexts — query the keychain, see
nothing, and conclude the token is broken. `sc_auth identities` and the device's
own `TRACE` are the reliable witnesses; `security list-smartcards` lags well
behind reality.

## Why there is no wireless

BLE was built and measured: the device served the same applet over the air,
`sudo` authenticated wirelessly, and a persistent CryptoTokenKit token signed
through `SecKeyCreateSignature`. It was removed because:

- **It cannot be driverless.** macOS has one smart-card transport and it is USB.
- **It cannot serve the login window.** No user session for CoreBluetooth and no
  TCC grant before login.
- **A registered wireless token starves a wired card.** A persistent token is
  always "present", so it wins against a reader-backed token permanently and
  silently. Measured: the wired card stopped authenticating entirely while an
  out-of-range wireless device held the slot.
- It adds a battery, a charger and a radio to a device you plug in.

The firmware ECDH, the AID mode switching and the driver architecture came out
of that work. The code is in the history.

## Provisioning a unit

```sh
./bootkeys.py primary.pem spare.pem build-rp2350/openhanko.uf2 out/
./provision-board.py out/            # rehearse, touching nothing
./provision-board.py out/ --commit
```

That takes a blank board to a finished one: signed firmware, both boot keys, the
bootrom's recovery, a device secret, key material encrypted at rest, secure boot,
and no debug port. Every OTP write is permanent, so the script verifies what
landed before continuing and refuses on any disagreement.

**The order is not arbitrary and not obvious.** The device burns its own secret
the first time it generates an identity, so it has to boot and run *before* page
4 is locked — lock first and it can never write one, and a device with no secret
cannot store an identity at all, which looks like a working board right up until
you try to pair it. Secure boot comes after that firmware is proven to boot on
that board. Debug goes last, because it is the step that removes the ability to
diagnose the others.

A run that stops partway leaves a real board in a real state, so the script
resumes by reading OTP rather than by being told where it got to.

### The two keys

Four boot key slots exist. Two are used and the other two are marked invalid,
which the datasheet asks for: otherwise somebody who later gets a write to OTP
can install a key of their own and sign what they like.

The second key is a recovery path, not a copy. If the primary leaks, revoke it
and sign with the spare — the fleet survives. With one key, a leak or a loss is
terminal for every unit ever burned. Verified on hardware: primary-signed
rejected after revocation, spare-signed still boots.

Keep the spare offline. It is worth nothing if it lives beside the primary.

### Recovery

`BOOT_FLAGS1.DOUBLE_TAP` is burned during stage 1, before secure boot, and it
matters that it is the bootrom's mechanism rather than the SDK's. The SDK's ran
in the application; enabling secure boot stops it working, and it needed the
firmware to boot — which is not when a way back in is wanted. The bootrom's runs
before any application, so it rescues a board whose firmware is broken, unsigned
or missing.

BOOTSEL and picotool keep working after lockdown. That is deliberate: a unit
stays updatable with signed firmware forever, while revealing nothing.

## Key material at rest

The PIV keys in flash are AES-256-GCM ciphertext under a key derived from a
32-byte secret the device generates for itself and burns into OTP. Reading the
flash yields nothing usable.

GCM rather than a bare cipher because the tag is what makes a bad decrypt
legible: without it a corrupted or tampered record decrypts to plausible bytes
that mbedTLS then tries to parse as a PEM key, and the complaint arrives about
the wrong thing entirely.

The secret is stored **chaffed** — every bit beside its own complement. That is
aimed at IOActive's passive voltage contrast attack, which reads antifuse cells
directly rather than through any access control and recovers the bitwise OR of
adjacent bits. It is the one Hacking Challenge finding A4 does not fix, and
complementary pairs are Raspberry Pi's published mitigation: an all-zero secret
and an all-ones secret produce byte-identical readouts.

Once the page is locked and debug disabled, that secret is readable by signed
firmware on that die and by nothing else — not SWD, not the bootloader, not
`picotool otp get`.

**Which makes the firmware the oracle.** It can read the secret; that is the
design. A bug that leaks it costs everything the rest of this bought, which is
why `OTP_STATUS` reports six bytes of a hash and never the value, and why
nothing should ever be added that returns it.

## The sensor link cannot be authenticated

Worth stating plainly, because it is the limit that no firmware here closes.

The ZW111 speaks the EF-01 / ZhianTec-family protocol over plain UART at 57600.
Confirmed opcodes, cross-checked against
[GavinnnTann/HLK-ZW-Fingerprint-Sensor](https://github.com/GavinnnTann/HLK-ZW-Fingerprint-Sensor)
and what this driver implements:

| opcode | command | implemented here |
| --- | --- | --- |
| `0x01` | `PS_GetImage` | yes |
| `0x02` | `PS_GenChar` — image to buffer | yes |
| `0x05` | `PS_RegModel` | yes |
| `0x06` | `PS_StoreChar` | yes |
| `0x0d` | `PS_Empty` — erase all templates | yes |
| `0x04` | `PS_Search` | yes |
| `0x0f` | `PS_ReadSysPara` | no |
| `0x12` | `PS_SetPwd` — persists to flash | no |
| `0x13` | `PS_VfyPwd` | yes |
| `0x14` | `PS_GetRandomCode` — module's own hardware RNG | yes |
| `0x16` | `PS_ReadINFpage` — parameter page; its Product SN is a **model** string | yes |
| `0x34` | `PS_GetChipSN` — 32-byte per-die serial | yes |
| `0x0e` | `PS_WriteReg` — register 7 is the encryption level | no |
| `0xe0`–`0xe4` | safety instruction set: key pair, lock, ciphertext, secure store/search | no |
| `0x1d` | `PS_TemplateNum` | yes |
| `0x3c` | `AURALEDCONFIG` — ZW1xx only | yes |

**As shipped, at encryption level 0, there is no authenticated mode.** The
4-byte password is not merely sent in clear on every power-up — it is *readable*
from the parameter page, so an attacker does not even need to be listening at
the right moment. A forged match response makes the device sign, and secure
boot, SWD lockout and encryption at rest all keep working correctly because none
of them are in that path.

**But the module can do better, and this is the open lead.** Hi-Link's protocol
manual documents a *safety instruction set* (`0xE0`–`0xE4`) and an encryption
level held in register 7:

| level | algorithm |
| --- | --- |
| 0 | none — everything except the safety set |
| 1 | none, and no template upload/download |
| 2 | SM4 (ECB) |
| 3 | AES-128 (ECB) |
| 4 | 3DES (ECB) |
| 20 | RSA-1024 |
| 21 | ECC P-256 |

At levels 2 and above the module refuses the ordinary commands entirely — no
`PS_Search`, no `PS_StoreChar`, no auto-registration — and answers only the
safety set, so enrolment and verification both move to `PS_SecurityStoreChar`
and `PS_SecuritySearch`. `PS_GetKeyt` generates the key material, and
**`PS_LockKeyt` then refuses to ever issue another pair**, which is what makes it
worth anything: provision at assembly, lock, and a later attacker cannot ask the
module for a fresh key.

Three things stand between that and a real defence, all unresolved:

- **Whether the ZW111 supports it at all.** The manual is written for Hi-Link's
  whole range and says only that "some fingerprint module products based on
  security chips" have it. Read register 7 and try `PS_GetKeyt`: `0x31` means
  the function does not match the encryption level.
- **The level is one-way.** "Changes are not allowed after setting." Set it wrong
  on a unit and that unit is what it is.
- **ECB.** Every symmetric level is ECB, which is deterministic and unauthenticated.
  Whether that is a channel worth trusting depends on how the challenge-response
  is constructed on top of it, and a random-nonce protocol can be sound over a
  bad mode — but it is not a detail to wave through.

Until then the honest claim is unchanged: **resists a compromised host, resists
offline key extraction, does not resist physical possession.**

What raises cost without pretending to be authentication:

- **Bind to the module.** `PS_GetChipSN` (`0x34`) returns a 32-byte serial the
  manual describes as unique to the die. Store a hash at pairing and refuse a
  module that changes. Defeats a swap with a stock module, not an emulator that
  replays the expected serial. **Implemented and in force**: the device records
  the module it finds when it has none stored, and refuses everything if it later
  meets a different one. Confirmed across two modules — five of the twelve
  meaningful bytes differ, so the serial is genuinely per-die.

  Not the info page's Product SN, which the manual defines as "indicate product
  model" — it names the part, not the unit, and binding to it would detect only a
  different *kind* of sensor. Both modules report the same one.
- **Correlate `TouchOut`.** *Implemented.* A match is discarded unless the line
  says a finger is present, checked before the capture and again after it.
- **Drive the full sequence.** Never trust one confirmation byte; run
  `GetImage → GenChar → Search` and check each stage against the last.
- **Bound the timing.** Reject responses that arrive implausibly fast. Bounds
  have to be measured across real modules, not guessed — capture latency varies
  with finger quality.

Note what is *not* on that list: deriving a key from a fingerprint template. The
ZW111 runs a self-learning algorithm, so templates mutate with use and no derived
key would be stable, and the template is readable by whoever holds the module, so
it is not secret either.

## Security posture

The full per-use-case analysis is in [THREAT-MODEL.md](THREAT-MODEL.md); the
short version:


- **A fingerprint proves an enrolled finger, not a person.** The sensor answers
  over an unauthenticated UART, so someone who opens the case can assert a match
  without one. Binding to the module's die stops the sensor being *swapped*;
  nothing yet stops it being *driven*.
- **The PIN is theatre.** `VERIFY` accepts any PIN and opens a window. macOS
  insists on collecting one; the fingerprint is the only real gate.
- **Keys are encrypted at rest**, under a key held in OTP rather than in flash,
  on a unit whose debug port is fused shut and which boots only signed firmware.
  What that leaves is the firmware itself: it can read the OTP secret by design,
  so a bug that leaks it costs everything the lockdown bought.
- **A touch authorises a session, not a single operation.** Slot 9a consumes its
  match per signature; slot 9d runs against a 60 s window that use does not
  consume, because macOS unwraps the login keychain there immediately after the
  9a signature that logged the user in.
- Every attack here needs physical access.

## Layout

```
src/                   device firmware, RP2350 family
  board_config.h       pins, AID default, timings
  main.c               cooperative loop: presence, indicator, mode switching
  piv.c                PIV applet: certificates, VERIFY, GENERAL AUTHENTICATE,
                       ECDH on slot 9D
  identity.c           generates the device's own keypair and certificate
  fingerprint.c        HLK-ZW111 over UART (EF-01), PS_AutoEnroll, module binding
  settings.c           which AID to answer, in its own flash sector
  storage.c            the PIV identity, in flash, encrypted, outside the image
  otp.c                the device secret that encrypts it, in one-time memory
  usb_ccid.c           CCID class driver over TinyUSB
  usb_hid.c            HID keyboard, for typing the PIN in standard mode
  config_console.c     provisioning and diagnostics on CDC
  trace.c              ring buffer of CCID and APDU activity
provision.py           console client: status, events, macOS pairing
```

The macOS driver and the site are in
[openhanko-macos](https://github.com/openhanko/openhanko-macos) and
openhanko-web.

## Next steps

1. **Wire a ZW111 and test `fingerprint.c`.** The search range and the LED
   parameters are the two things most likely to need adjusting.
2. **Port to RP2350** and regenerate every identity there.
3. **Burn OTP and lock debug**, last, on a unit you can afford to brick. This is
   the step that turns the fingerprint from a convenience into a security
   property.
