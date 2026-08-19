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

Not done:

- `fingerprint.c` is written against the EF-01 protocol but **untested against a
  module**. A board with no sensor reports `fp=absent` and falls back to the
  button, so one image serves both.
- Not yet ported to RP2350. Keys generated on an RP2040 are development-only.
- Secure boot, OTP and debug lockout are not enabled.

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
| MCU | ESP32-S3 | RP2040; RP2350 is the target |
| signing algorithms | RSA-2048 only — the sign path returns `6f00` for any key that is not RSA, and `GENERAL AUTHENTICATE` rejects any `P1` but `0x07` | P-256 (`0x11`) and RSA-2048 (`0x07`), checked against the key actually loaded |
| signature time | — | 469 ms P-256, 2924 ms RSA-2048, on an RP2040 |
| ECDSA nonce | n/a | RFC 6979 deterministic |
| where the key comes from | provisioned from a host, or compiled into the image | **generated on the device at first boot**; both host paths kept as alternatives |
| does the private key exist off-device | yes — on whatever machine generated it | no, on the generate-on-device path |
| slot 9D key agreement | not implemented | ECDH P-256, dynamic auth tag `0x85` |
| pinpad PIN entry | not implemented | `bPINSupport` declared and `PC_to_RDR_Secure` answered from a press |
| AIDs answered | standard PIV only | standard PIV **and** a private AID |
| driver detection | none | private-AID probe: the device works out for itself whether its driver is installed, and switches mode both ways |
| host software | Python LaunchAgent, AES/HMAC over HID | signed and notarised CryptoTokenKit extension |
| factory reset without a host | no | button held through power-up, release commits |
| on-device diagnostics | `STATUS` | `STATUS`, plus a `TRACE` ring buffer of CCID and APDU activity and `BENCH` |
| fingerprint sensor | ZW101, working | ZW111, written and **untested** |

### What upstream has that this does not

- **A fingerprint driver that has met a sensor.** `fingerprint.c` here is written
  from the EF-01 protocol and has never been run against hardware. Upstream's
  has.
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
driver present, our extension binds and the press alone signs with no dialog at
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

| part | role |
| --- | --- |
| **RP2350** | target: secure boot, OTP debug lockout, hardware TRNG |
| **RP2040** | development; every measurement below is from this part |

`rp2040/` builds for both. The RP2350 matters for keys rather than speed: it
verifies a signed image at boot, permanently disables SWD through OTP, and seeds
`get_rand_64()` from a real TRNG. The RP2040 does none of these — its debug port
is always open, so anyone who opens the case reads the key out and steps around
the sensor entirely.

### Pinout

| function | pin |
| --- | --- |
| fingerprint module | **GP4** TX, **GP5** RX — UART1, 57600, HLK-ZW111 |
| presence button | **GP10** to GND, internal pull-up |
| indicator LED | **GP16**, WS2812 — redundant once a sensor is fitted, which has its own ring |

The ZW111 harness is six wires, not four:

| ZW111 pin | to |
| --- | --- |
| 1 `V_Touch` | 3V3 — **permanently powered**, this is what runs finger detection while the rest of the module sleeps |
| 2 `TouchOut` | not currently wired; asserts on finger contact |
| 3 `VCC` | 3V3 |
| 4 `TX` | GP5 (module → MCU) |
| 5 `RX` | GP4 (MCU → module) |
| 6 `GND` | GND |

`TouchOut` is the one optional wire. Correlating it with the UART match response
costs a GPIO and raises the bar for forging a match from "drive RX" to "drive RX
and a second line in a plausible time relationship". Everything about that link
is cost-raising rather than authentication — see [the sensor link](#the-sensor-link-cannot-be-authenticated).

### Which stepping am I holding?

`STATUS` reports it as `chip=`, e.g. `chip=rp2350-a4` or `chip=rp2040-b2`.

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

`BOOT_FLAGS0.DISABLE_WATCHDOG_SCRATCH` is the documented mitigation for E20 —
but it also disables the watchdog scratch register that
`pico_bootsel_via_double_reset` depends on. Setting it on A3 or A4 buys nothing,
since E20 is already fixed there, and costs the double-tap-RESET route back into
the bootloader. With SWD locked, that is the only route.

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

Defaults live in [`rp2040/board_config.h`](rp2040/board_config.h). For a bare
board with nothing wired, set `BUTTON_USE_BOOTSEL 1` to borrow the BOOTSEL
button. BOOTSEL sits on the QSPI bus rather than the GPIO bank, so reading it
overrides the flash chip-select and blacks out interrupts for tens of
microseconds; sampling is rate-limited to 20 ms to keep that off USB's back.
Bench use only.

## Build and flash

```sh
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0
cd rp2040 && cmake -S . -B build && cmake --build build
```

Hold BOOTSEL while replugging, then copy the image onto the volume that appears:

```sh
cp -X build/smart_card_rp2040.uf2 /Volumes/RPI-RP2/
```

**Double-tap RESET** within 800 ms to enter the bootloader without the BOOTSEL
hold (`pico_bootsel_via_double_reset`). It relies on a watchdog scratch register
surviving the reset, so it needs a real RESET button — a power cycle will not
trigger it.

A fresh clone needs `PICO_SDK_PATH` set explicitly. An existing `build/`
directory caches it, so a missing SDK only surfaces on a clean checkout.

## Identity

A device with no identity generates a P-256 keypair and self-signed certificate
at first boot. The private key never leaves the chip.

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

`get_rand_64()` is the limit. On the RP2040 it is ring-oscillator jitter, not a
trustworthy source for long-lived keys. On the RP2350 the same call is backed by
the hardware TRNG.

### Two other ways to load one

`./provision.py` is stdlib-only — no pip install, no venv.

```sh
./provision.py provision              # generate on your Mac, push over CDC into flash
./provision.py gen-secrets            # generate and write rp2040/secrets.h, then reflash
```

`provision` writes no private key to disk unless you pass `--keep-keys DIR`.
`gen-secrets` bakes keys into the image, so every device flashed from it shares
one identity — convenient for bench work, wrong for anything else. `secrets.h`
is gitignored; if it is absent the firmware builds fine and waits, and if it
still holds the `REPLACE_WITH` placeholders it is ignored with a warning.

Precedence is flash over compiled-in. `STATUS` reports which won as
`source=flash` or `source=compiled`.

Both paths take `--algorithm ec` (default, P-256) or `--algorithm rsa`
(RSA-2048). The firmware supports both and checks the host's `P1` against the
key actually loaded.

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
sudo -k && sudo -v          # test: press the button when macOS asks for the PIN
```

macOS usually offers to pair on its own when the card is inserted; the "Unpaired
SmartCard inserted" notification does the same thing as `pair`. `pair` reads the
identity hash out of `sc_auth identities` and prints the `sudo sc_auth pair`
command to run, or runs it with `--run`.

`./provision.py monitor` prints device events live, which is the fastest way to
tell whether a press registered.

## Console commands

CDC console, `115200`. `./provision.py console '<CMD>'` sends one.

| command | effect |
| --- | --- |
| `PING` | → `PONG` |
| `STATUS` | silicon stepping, key source, algorithm, AID mode, pairing, sensor, name |
| `TRACE` / `TRACE_CLEAR` | ring buffer of CCID and APDU activity |
| `BENCH` | time one signature with the loaded key |
| `GENERATE_IDENTITY` | new on-device keypair and certificate |
| `PROVISION_BEGIN` / `PROVISION_COMMIT` | staged identity upload, used by `provision.py` |
| `ENROLL <n>` / `FINGERPRINT_ERASE` | fingerprint template management |
| `FINGERPRINT_INFO` | module serial, firmware, manufacturer, sensor name |
| `FINGERPRINT_INFO_RAW` | the raw 512-byte info page as hex, for checking the field offsets |
| `PAIRING_MODE` / `PAIRING_MODE_OFF` | sign without a press, for pairing flows |
| `BOOTLOADER` | reboot to USB mass-storage bootloader |
| `REBOOT`, `USB_RECONNECT` | as named |
| `CONFIG_UNLOCK` | required before destructive commands |

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
  the press types the PIN    │  nothing else knows to ask for it
                             ▼
                   persist pinpad, reboot
                             │
pinpad mode                  ▼
  answers the private     our driver binds exclusively;
  AID only                the press alone signs, LED breathing
                             │
                             │  no private-AID select within 10 s
                             │  of the host first speaking
                             ▼
                   persist standard, reboot
```

The probe is the mechanism: only our driver knows to ask for the private AID, so
a SELECT of it proves the driver is installed. The device is never told.

All four transitions measured on an RP2040:

| behaviour | evidence |
| --- | --- |
| driverless by default | `aid=standard`, `pivtoken` binds, 580 ms press→signature |
| upgrades on meeting its driver | forced to standard; back to `aid=pinpad claimed=yes` in under four seconds, unattended |
| pinpad authenticates | `sudo` on a press, no PIN typed |
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

```
TRACE 35951 APDU ins=a4 sw=9000              driver selects the private AID
TRACE 35951 APDU ins=87 p1=11 p2=9a sw=6982  sign attempted, authentication needed
TRACE 35957 CCID 69 Secure x1                PC_to_RDR_Secure: the pinpad request
TRACE 39362 EVENT BUTTON                     the press
TRACE 39831 APDU ins=87 p1=11 p2=9a sw=9000  signed, 469 ms later
```

No `ins=20` VERIFY: no PIN is typed or transmitted. The `6982` is what makes
CryptoTokenKit call `beginAuth`, and `CCID 69` is the secure-PIN request that
identifies our driver.

### Standard mode

```
TRACE 428603 EVENT BUTTON                       the press
TRACE 428722 APDU ins=20 … sw=9000              VERIFY, the PIN the device typed
TRACE 429183 APDU ins=87 p1=11 p2=9a sw=9000    signed
```

580 ms press to signature, with nothing installed. macOS labels the prompt
*"Certificate For PIV Authentication (…)"*, which is `pivtoken`'s own format.

The PIN is `000000` and is not a secret — `VERIFY` accepts anything. The button
is the only gate.

## Indicator

| mode | behaviour |
| --- | --- |
| pinpad | **breathes** while waiting — macOS shows no prompt, so this is the entire invitation |
| standard | **solid flash**, 700 ms, on a press, held through the signature |

The asymmetry is inherent. **macOS says nothing to the card until a PIN has
already been submitted** — measured three times, including an empty trace with a
prompt on screen for ten seconds:

```
CCID 62 IccPowerOn
CCID 63 IccPowerOff      7.5 s later, nothing asked
```

So in standard mode there is no event to light up on; the device can only
acknowledge a press after the fact. A button that swallows presses without a
flicker reads as broken, especially when the PIN it typed lands in a window the
user is not looking at.

A WS2812 latches its last value, so a board left glaring by earlier firmware
stays that way until something clears it — hence the boot-time clear.

## Implementation notes

### Use P-256, not RSA-2048

| `BENCH` signing | RSA-2048 | ECC P-256 |
| --- | ---: | ---: |
| RP2040 @133 MHz | 2924 ms | **469 ms** |

| end to end on an RP2040 | RSA-2048 | ECC P-256 |
| --- | ---: | ---: |
| press to authenticated | ~2960 ms | **605 ms** |
| APDU transfers for the challenge | 2 (chained) | 1 |
| compiled identity (`secrets.h`) | 6.2 kB | 3.4 kB |

RSA is slow because the RP2040 has no big-integer accelerator and the Cortex-M0+
has no 64-bit multiply. Signing runs synchronously inside the CCID transfer
callback, so USB stalls for the whole operation. macOS waited patiently even at
three seconds, so this is comfort rather than correctness.

macOS picks the algorithm from the certificate with no configuration: a P-256
cert gets `GENERAL AUTHENTICATE` with `P1 = 0x11` instead of `0x07`.

### ECDSA is RFC 6979 deterministic

`MBEDTLS_ECDSA_DETERMINISTIC`, and not optional. The RP2040 has no hardware
TRNG, and a predictable ECDSA nonce recovers the private key from a single
signature. Deriving the nonce from the key and message takes the RNG out of the
signing path entirely.

### EC keys must use named-curve encoding

LibreSSL's `openssl req -newkey ec` writes **explicit** EC parameters by default
— field type, prime, generator, the lot — and mbedTLS rejects that with
`MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE` (`-0x4E80`) unless
`MBEDTLS_PK_PARSE_EC_EXTENDED` is enabled.

`./provision.py` passes `-pkeyopt ec_param_enc:named_curve`, which is what real
PIV cards carry and shrinks the key from 377 to 135 bytes.

The failure mode is confusing: the certificate decodes, the card enumerates,
macOS reads the identity, and only signing fails. `STATUS` reports `alg=` and
`keyrc=` so this is one command away from a diagnosis.

### Omitting `MBEDTLS_PEM_PARSE_C` fails the same way

Certificates still work, because `decode_pem_cert()` in `piv.c` does its own
base64. The card enumerates, macOS offers to pair, and only signing fails — with
`6f00`. `TRACE` pinned it by showing `6f00` where a missing press would have
given `6982`.

### Apple's `pivtoken` ignores pinpad entirely

CCID readers can declare `bPINSupport` and collect the PIN themselves, which
would mean no text field, no keystrokes, and nothing to focus. CryptoTokenKit
does read that byte out of the CCID descriptor.

With Apple's built-in `pivtoken`: **not a single `PC_to_RDR_Secure`**, even with
`bPINSupport = 0x01` declared.

With the driver in [openhanko-macos](https://github.com/openhanko/openhanko-macos)
it works — no dialog, nothing typed, authenticated by the button alone, 483 ms
end to end.

`sudo` is seamless, including the prompt: that repository's `tools/pam` module
runs ahead of `pam_smartcard.so` and goes through CryptoTokenKit, so the token
driver performs the authentication and the press is the whole interaction. A
`sudo` traced on this device shows `CCID 69 Secure`, the press, and the
signature, with **no `VERIFY` at all** — nothing was typed.

What is *not* solved is applications that put up their own PIN field. Chrome's
password manager unlocks correctly but still shows a modal and takes the typed
`000000`; pinpad governs the CryptoTokenKit-to-card leg, and an application that
collects a PIN itself never reaches it.

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
  silently. Measured: the RP2040 stopped authenticating entirely while an
  out-of-range wireless device held the slot.
- It adds a battery, a charger and a radio to a device you plug in.

The firmware ECDH, the AID mode switching and the driver architecture came out
of that work. The code is in the history.

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
| `0x0f` | `PS_ReadSysPara` | no |
| `0x12` | `PS_SetPwd` — persists to flash | no |
| `0x13` | `PS_VfyPwd` | yes |
| `0x14` | `PS_GetRandomCode` — module's own hardware RNG | yes |
| `0x16` | `PS_ReadINFpage` — 512-byte info page, carries a product serial | yes, untested |
| `0x1b` | fast search — ZW1xx only, ZW30xx answers `0x13` | yes |
| `0x1d` | `PS_TemplateNum` | yes |
| `0x3c` | `AURALEDCONFIG` — ZW1xx only | yes |

There is **no encrypted or authenticated mode**. The 4-byte password is sent in
clear on every power-up, so it is capturable on the first transaction; it is a
speed bump, not a secret. An attacker holding the device can cut the harness and
speak the protocol directly, and the module will tell them anything it would
tell us.

So a forged match response makes the device sign. Secure boot, SWD lockout, OTP
protection and encryption at rest all keep working correctly — they are simply
not in that path. **The honest claim is: resists a compromised host, resists
offline key extraction, does not resist physical possession.**

What raises cost without pretending to be authentication:

- **Bind to the module.** `PS_ReadINFpage` (`0x16`) returns a product serial;
  store a hash of it at pairing and refuse a module that changes. Defeats a swap
  with a stock module, not an emulator that replays the expected serial.
  `0x16` is implemented and reachable as `FINGERPRINT_INFO`, but **untested**,
  and binding is not wired up — see the open question below.

  **The open question is whether `product_sn` is per-unit or per-model.** If it
  names the model, binding to it detects a different *kind* of sensor and
  nothing else, which is far weaker than it sounds. Two modules from the same
  reel will answer that on the bench in a minute; until then, treat module
  binding as unproven rather than pending.
- **Correlate `TouchOut`.** Require the touch line to assert in a plausible
  relationship to the UART response.
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


- **A button proves presence, not identity.** While the trigger is the button,
  anyone who can reach the device can authenticate as you. The fingerprint sensor
  is what changes that, and it is untested against hardware.
- **The PIN is theatre.** `VERIFY` accepts any PIN and opens a window. macOS
  insists on collecting one; the presence check is the only real gate.
- **Keys are plaintext in flash** and come straight out over SWD. Closing this is
  what the RP2350's secure boot and debug lockout are for, and none of it is
  enabled yet.
- **A press authorises a session, not a single operation.** Slot 9a consumes its
  press per signature; slot 9d runs against a 60 s window that use does not
  consume, because macOS unwraps the login keychain there immediately after the
  9a signature that logged the user in.
- Every attack here needs physical access.

## Layout

```
rp2040/                builds for RP2040 and RP2350
  board_config.h       pins, AID default, timings
  main.c               cooperative loop: presence, indicator, mode switching
  piv.c                PIV applet: certificates, VERIFY, GENERAL AUTHENTICATE,
                       ECDH on slot 9D
  identity.c           generates the device's own keypair and certificate
  fingerprint.c        HLK-ZW111 over UART (EF-01) — untested against hardware
  settings.c           which AID to answer, in its own flash sector
  storage.c            the PIV identity, in flash, outside the image
  usb_ccid.c           CCID class driver over TinyUSB
  usb_hid.c            HID keyboard, for typing the PIN in standard mode
  config_console.c     provisioning and diagnostics on CDC
  trace.c              ring buffer of CCID and APDU activity
provision.py           key generation, provisioning, macOS pairing
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
