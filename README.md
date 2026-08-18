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
substantially upstream's work.

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

### USB identity

| | |
| --- | --- |
| VID | `0x16D0` — MCS Electronics |
| PID | `0x1551` |
| serial | `SC-` + the board's unique id, at runtime |
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

`FACTORY_RESET` over the console destroys the key, restores every setting to its
default and reboots, generating a fresh identity on the way back up.

Without a host: hold the button through power-up. The indicator blinks, faster
as the deadline approaches, and goes solid at six seconds; **releasing** is what
commits. Let go early, or keep holding, and nothing happens — once solid,
unplugging while still holding is the way out.

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
| `STATUS` | firmware, key source, algorithm, AID mode, pairing, sensor, name |
| `TRACE` / `TRACE_CLEAR` | ring buffer of CCID and APDU activity |
| `BENCH` | time one signature with the loaded key |
| `GENERATE_IDENTITY` | new on-device keypair and certificate |
| `PROVISION_BEGIN` / `PROVISION_COMMIT` | staged identity upload, used by `provision.py` |
| `ENROLL <n>` / `FINGERPRINT_ERASE` | fingerprint template management |
| `PAIRING_MODE` / `PAIRING_MODE_OFF` | sign without a press, for pairing flows |
| `FACTORY_RESET` | destroy the key, clear settings, reboot |
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
end to end. `sudo` is the exception: `pam_smartcard` collects a PIN before
CryptoTokenKit is consulted and hands it over pre-filled, so the prompt still
appears and the device still types `000000` there.

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

## Security posture

- **A button proves presence, not identity.** While the trigger is the button,
  anyone who can reach the device can authenticate as you. The fingerprint sensor
  is what changes that, and it is untested against hardware.
- **The PIN is theatre.** `VERIFY` accepts any PIN and opens a window. macOS
  insists on collecting one; the presence check is the only real gate.
- **Keys are plaintext in flash** and come straight out over SWD. Closing this is
  what the RP2350's secure boot and debug lockout are for, and none of it is
  enabled yet.
- **Slot 9d is not presence-gated.** Anything that reaches the device can run a
  key agreement against it. Deliberate — macOS unwraps the login keychain there
  right after a press the user already made — but it means a press authorises a
  session, not a single operation.
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
