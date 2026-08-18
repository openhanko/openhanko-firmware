# openhanko-firmware

Firmware for [OpenHanko](https://openhanko.io): a small USB device that unlocks
a Mac the way Touch ID does — login, `sudo`, authorisation prompts — on a Mac
that hasn't got one. It enumerates as a PIV smart card over USB CCID, and macOS
authenticates against a private key the device generated itself and will only
use when someone physically touches it.

```
$ system_profiler SPSmartCardsDataType
    Readers:
      #01: OpenHanko Smart Card (ATR:{length = 4, bytes = 0x3b800101})
```

## Hardware

**Target: RP2350**, with an HLK-ZW111 capacitive fingerprint module as the
presence check. `rp2040/` builds for both parts — the RP2350 is pin-compatible
in the ways that matter here and the port is small.

| | role | state |
| --- | --- | --- |
| **RP2350** | what ships | target; secure boot, OTP, real TRNG |
| **RP2040** | development | everything below is measured on this |
| **ESP32-S3** | where it started | `simple/`, kept and working, not the target |

RP2350 is the shipping part for reasons that are about keys rather than speed.
It can verify a signed image at boot, permanently disable SWD through OTP, and
generate keys from a hardware TRNG. The RP2040 can do none of those: its debug
port is always open, so anyone who opens the case reads the key out and the
fingerprint sensor is simply stepped around. **A key generated on an RP2040 is
for development.** That distinction is what makes the sensor worth fitting at
all.

| | RP2040 | ESP32-S3 |
| --- | --- | --- |
| signing | ECDSA P-256, **469 ms** | ECDSA P-256, **162 ms** |
| touch to authenticated | ~0.6 s | ~0.7 s |
| presence | GP10 button, ZW111 on UART1 | GPIO0 / BOOT |
| indicator | the sensor's own LED ring | WS2812 GPIO48 |

The ESP32-S3 build is kept because it works and because the applet is shared,
not because anything is planned for it. Why it was chosen originally, and why
no other ESP32 can do this, is in [the history](#why-it-started-on-an-esp32-s3)
below — the analysis is still correct and still useful if you ever want to put
this on that family.

## Status

Working on hardware, end to end. macOS enumerates it, attaches a PIV token
driver, reads the certificate, pairs it to the account, and authenticates
`sudo` and the login window against it. The device generates its own key at
first boot, names itself after that key, and can erase itself back to factory
state without a host.

The fingerprint driver in `rp2040/fingerprint.c` is written against the EF-01
protocol but **not yet tested against a module** — the sensors are on order. A
board with no sensor attached reports `fp=absent` and keeps the button as the
trigger, so one firmware serves both.

This is derived from [ZimengXiong/tinyTouch](https://github.com/ZimengXiong/tinyTouch)
(`firmware/tiny_touch_smartcard`). The PIV applet and CCID transport are
substantially upstream's work.

## Wireless was built, measured, and dropped

BLE worked. The device served the same PIV applet over the air, `sudo`
authenticated wirelessly with no change to the PAM module, and a persistent
CryptoTokenKit token signed through `SecKeyCreateSignature`. The code is in the
history if anyone wants it.

It was removed anyway, for reasons that are worth stating because they are not
obvious until you have built it:

- **It cannot be driverless.** macOS has one smart-card transport and it is USB,
  so a wireless device is useless until the user installs an extension. The
  whole appeal of the wired build is that it works on an untouched Mac.
- **It cannot serve the login window.** Before login there is no user session
  for CoreBluetooth and no TCC grant, so the lock screen stays wired regardless.
- **A registered wireless token starves a wired card.** A persistent token is
  always "present" — there is no card to remove — so it wins against a
  reader-backed token permanently, and silently. Measured: the RP2040 stopped
  authenticating entirely while an out-of-range ESP32 held the slot.
- **It adds a battery, a charger, and a radio** to a device whose selling point
  is that you plug it in and it works.

The measurements it produced are kept below where they still apply — the
firmware ECDH, the AID mode switching, and the driver architecture all came out
of that work.

## Mode switching: automatic in both directions

One firmware image, no reflash, no installer step, and nothing for the user to
configure. The device works out of the box, upgrades itself when it meets its
driver, and rescues itself when it does not.

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

All four transitions measured on an RP2040:

| behaviour | evidence |
| --- | --- |
| driverless by default | `aid=standard`, `pivtoken` binds, 580 ms press→signature |
| upgrades on meeting its driver | forced to standard; back to `aid=pinpad claimed=yes` in under four seconds, unattended |
| pinpad authenticates | `sudo` on a press, LED breathing, no PIN typed — trace below |
| reverts when the driver is gone | `aid=pinpad claimed=yes` → nothing registered → `aid=standard claimed=no` |

The probe is what makes this work. Only our driver knows to ask for the private
AID, so a select of it is proof the driver is installed — the device does not
have to be told.

**Why answering both AIDs is safe here and not in general.** A card that
*stays* in that state leaves two drivers in the running, and macOS binds exactly
one per card at insertion: we measured that race flipping between reboots, and
saw `sc_auth` file one identity under "not used for authentication" while that
card silently stopped authenticating. Here the state lasts a few hundred
milliseconds before the reboot makes it exclusive. Checked afterwards, the
identity is "used for authentication" and `sudo` works — the transient leaves no
residue.

**One consequence to know:** with the driver installed, forcing standard mode
does not stick, because the device meets the driver again and upgrades straight
back. Driverless behaviour on a Mac that has the driver means uninstalling it.
The button-held-at-boot override remains for the cases automation cannot reach,
though the automatic revert now covers nearly all of them.

## Pinpad mode, verified

With the driver installed and the device in pinpad mode, `sudo` completes on a
button press with no prompt and no PIN anywhere:

```
TRACE 35951 APDU ins=a4 sw=9000              driver selects the private AID
TRACE 35951 APDU ins=87 p1=11 p2=9a sw=6982  sign attempted, authentication needed
TRACE 35957 CCID 69 Secure x1                PC_to_RDR_Secure: the pinpad request
TRACE 39362 EVENT BUTTON                     the press
TRACE 39831 APDU ins=87 p1=11 p2=9a sw=9000  signed, 469 ms later
```

Note what is absent: no `ins=20` VERIFY, because no PIN is typed or transmitted.
The `6982` is what makes CryptoTokenKit call `beginAuth`, and `CCID 69` is the
secure-PIN request Apple's `pivtoken` never sends — which is both what lights
the LED and how the device knows our driver is the one talking to it.

## Driverless mode, verified

A factory-default device answers the standard PIV AID, so Apple's built-in
`pivtoken` binds and nothing has to be installed. Traced on an RP2040 during a
`sudo` on a Mac with no extension involved:

```
TRACE 428603 EVENT BUTTON                       the press
TRACE 428722 APDU ins=20 … sw=9000              VERIFY, the PIN the device typed
TRACE 429183 APDU ins=87 p1=11 p2=9a sw=9000    signed
```

580 ms from press to signature. macOS labels the prompt *"Certificate For PIV
Authentication (…)"*, which is `pivtoken`'s own format — proof the built-in
driver served it.

The indicator works differently in each mode, and the asymmetry is inherent
rather than an implementation gap.

In pinpad mode the LED **breathes**: macOS shows no prompt at all, so that light
is the entire invitation to act.

In standard mode it cannot invite anything. The card hears nothing until a PIN
has already been submitted — measured three separate times, including an empty
trace while a PIN prompt sat on screen — so there is no event to light up on.
What it can do is **acknowledge**: a press produces a solid flash for 700 ms,
held through the signature that follows. A button that swallows presses without
a flicker reads as broken, especially when the PIN it typed lands in a window
the user is not looking at.

So: the device can always say "I heard you", and can only say "press now" when a
driver is there to tell it.

### A note on testing

Smart-card token keys are only visible from a process with a full user session.
Automated shells (CI, agents, some launchd contexts) query the keychain and see
nothing, then conclude the token is broken — a false negative that cost real
time here. Persistent tokens do not behave this way, which makes the difference
easy to miss. `sc_auth identities` and the device's own `TRACE` are the reliable
witnesses; `security list-smartcards` also lags well behind reality.

## Factory reset without a host

Hold the button through power-up. The indicator blinks, accelerating as the
deadline approaches, and goes solid at six seconds; **releasing** is what
commits. Let go early, or keep holding, and nothing happens. Once it is solid,
unplugging while still holding is the way out.

The release is the commit deliberately. A device wedged against something in a
bag can hold a button indefinitely but cannot let go — and with no screen, an
accelerating blink is the only warning available that something irreversible is
about to happen.

Measured on hardware: `OpenHanko #539755` → gesture → `OpenHanko #FA764A`, with
`source=flash` and `aid=standard`. The key is destroyed, every setting returns
to its default, and a fresh identity is generated on the way back up, so the
device comes back as though it had never been used.

There is no forced-standard-mode gesture. It was redundant once the automatic
revert was verified: a device meeting a Mac without the driver recovers on its
own, and on a Mac that *has* the driver forcing standard mode would not stick
anyway, since the probe upgrades it straight back.

## Identity: generated on the device, named after itself

A device with no identity generates its own P-256 keypair and self-signed
certificate at first boot. The private key never exists outside the chip —
nothing to store on a workshop machine, nothing to back up, nothing to leak, and
no need to ask anyone to trust what the vendor did with it.

Certificates are named after the tail of the public key's SHA-1, which is also
how macOS derives `kSecAttrApplicationLabel` — the hash `sc_auth` pairs against.
Measured, not assumed:

```
$ STATUS
… source=flash alg=p256 keyrc=-0x0000 name="OpenHanko #FA764A"

$ sc_auth identities
CD01D8E61DB4E82E005F560781284FCE79FA764A   vden - Certificate For PIV Authentication (OpenHanko #FA764A)
                                  ^^^^^^
```

So a device states its own name over the console, and that name identifies its
entry on any Mac it is paired to. Two devices on one machine are told apart
without unplugging either.

The RNG is the limit, and it is a real one. This leans entirely on
`get_rand_64()`, which on the RP2040 is ring-oscillator jitter and not a
trustworthy source for long-lived keys — **keys generated on an RP2040 are for
development**. On the RP2350 the same call is backed by the hardware TRNG, and
only there is this sound.

`FACTORY_RESET` destroys the key, restores every setting to its default and
reboots, so a fresh identity is generated on the way back up. That covers both
proving a unit before boxing it and handing one to somebody else — the second
needs the key destroyed rather than merely unpaired, or the device still
authenticates as its previous owner wherever they paired it.

### ESP32-S3 wiring

- Any ESP32-S3 board with the native USB pins (GPIO19/20) on a connector.
  Seeed XIAO ESP32-S3 and ESP32-S3-DevKitC-1 both work.
- A momentary button. **GPIO0 is the BOOT button on nearly every S3 board**, so
  the default configuration needs no wiring at all.

Change the pin in [`simple/main/board_config.h`](simple/main/board_config.h):

```c
#define BUTTON_GPIO 0
#define BUTTON_ACTIVE_LEVEL 0   // 0 = switch to GND, 1 = TTP223 touch module
#define BUTTON_PULL_UP 1
```

A capacitive TTP223 breakout is a plain digital output, so it drops in with
`ACTIVE_LEVEL 1` and `PULL_UP 0` — no code change.

### Status LED

If the board has a WS2812, the firmware drives it off at boot and breathes it
while the device is waiting for a press — either macOS is mid-authentication or
a console command asked for one.

```c
#define STATUS_LED_GPIO 48        // 38 on DevKitC-1 v1.1, 21 on S3-Zero, -1 to disable
#define STATUS_LED_BRIGHTNESS 64  // peak, out of 255 — full scale is blinding
#define STATUS_LED_BREATHE_MS 2000
```

A WS2812 latches whatever it was last sent, so a board left glaring green by
earlier firmware stays that way until something clears it. That is what the
boot-time clear is for.

Using GPIO0 means holding the button during reset enters firmware download mode.
That is convenient now and worth moving off later.

## RP2040 and RP2350

`rp2040/` is the shipping line. The same applet on a Raspberry Pi part —
cheaper, smaller, and with no radio, which costs nothing now that wireless is
gone. It builds for the RP2350 as well, which is what units are assembled from:
the difference that matters is not speed but that an RP2350 can lock its debug
port and generate keys from a real TRNG.

```sh
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0
cd rp2040 && mkdir -p build && cd build
cmake -G Ninja .. && ninja
# hold BOOTSEL, replug, then:
cp -X smart_card_rp2040.uf2 /Volumes/RPI-RP2/
```

| function | pin |
| --- | --- |
| fingerprint module | **GP4** TX, **GP5** RX (UART1, 57600) — HLK-ZW111 |
| presence button | **GP10** to GND (internal pull-up, no resistor) |
| indicator LED | **GP16**, WS2812 — redundant once a sensor is fitted, since the module has its own ring |

**Double-tap RESET** within 800 ms to enter the bootloader — `pico_bootsel_via_double_reset`
from the SDK, so reflashing needs neither the BOOTSEL hold nor an unlocked
console. It relies on a watchdog scratch register surviving the reset, so it
needs a real RESET button; a power cycle will not trigger it.

The LED is dark in normal use and breathes **only while the reader is waiting
for a press**. That matters more than it sounds: with pinpad PIN entry macOS
shows no prompt at all, so without an indicator the device waits in silence with
nothing for the user to react to.

For a bare board with nothing wired, set `BUTTON_USE_BOOTSEL 1` in
`board_config.h` to borrow the BOOTSEL button instead. That costs an interrupt
blackout per sample, since BOOTSEL sits on the QSPI bus rather than the GPIO
bank, so sampling is rate-limited to 20 ms. Fine for bench work, not for
anything permanent.

### What changed from the ESP32 build

| | ESP32-S3 | RP2040 |
| --- | --- | --- |
| concurrency | FreeRTOS tasks | one cooperative loop |
| key storage | NVS | CRC-checked record in the top 3 flash sectors |
| entropy | `bootloader_random_enable()` | `pico_rand` (ROSC — weaker) |
| enter bootloader | `RTC_CNTL_FORCE_DOWNLOAD_BOOT`, ~50% reliable | `reset_usb_boot()`, always works |
| console/logs | shares one USB PHY | UART0, fully independent of USB |

The CCID, HID, PIV and console layers port essentially verbatim — TinyUSB *is*
the pico-sdk USB stack, so the class driver needed no changes at all.

### Signing: use ECC P-256, not RSA-2048

Measured on hardware with the `BENCH` console command, and confirmed end to end
in the device trace:

| `BENCH` signing | RSA-2048 | ECC P-256 |
| --- | ---: | ---: |
| RP2040 @133 MHz | 2924 ms | **469 ms** |
| ESP32-S3 @240 MHz | hardware MPI | **162 ms** |

| on the RP2040 | RSA-2048 | ECC P-256 |
| --- | ---: | ---: |
| press to authenticated | ~2960 ms | **605 ms** |
| APDU transfers for the challenge | 2 (chained) | 1 |
| compiled identity (`secrets.h`) | 6.2 kB | 3.4 kB |

macOS picks the algorithm from the certificate with no configuration: given a
P-256 cert it sends GENERAL AUTHENTICATE with `P1 = 0x11` instead of `0x07`.

```
48342 ms  EVENT BUTTON
48473 ms  APDU 00 20 00 80  VERIFY        9000
48947 ms  APDU 00 87 11 9A  GENERAL AUTH  9000   <- 0x11 = ECC P-256
```

Generate an ECC identity with `--algorithm ec` (the default). Both firmware
targets accept either algorithm and check the host's `P1` against the key
actually provisioned, so `--algorithm rsa` still works if you want it.

RSA is slow here because the RP2040 has no big-integer accelerator and the
Cortex-M0+ has no 64-bit multiply. Signing also runs synchronously inside the
CCID transfer callback, so USB stalls for the whole operation — three seconds of
that is long enough to be worrying, half a second is not. (macOS waited patiently
even at three seconds, so this was always comfort rather than correctness.)

The firmware supports both algorithms and picks based on the key actually
provisioned, checking it against the `P1` the host asks for.

**ECDSA here is RFC 6979 deterministic** (`MBEDTLS_ECDSA_DETERMINISTIC`), and
that is not optional: the RP2040 has no hardware TRNG, and a predictable ECDSA
nonce recovers the private key from a single signature. Deriving the nonce from
the key and message takes the RNG out of the signing path entirely.

### EC keys must use named-curve encoding

LibreSSL's `openssl req -newkey ec` writes **explicit** EC parameters by default
— field type, prime, generator, the whole curve — and mbedTLS rejects that with
`MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE` (`-0x4E80`) unless
`MBEDTLS_PK_PARSE_EC_EXTENDED` is enabled.

The failure looks exactly like the missing-PEM-parser one: the certificate
decodes, the card enumerates, macOS reads the identity, and only signing fails.
`./provision.py` therefore passes `-pkeyopt ec_param_enc:named_curve`, which
is what real PIV cards carry anyway and shrinks the key from 377 to 135 bytes.

`STATUS` reports `alg=` and `keyrc=` precisely so this class of failure is one
command away from a diagnosis rather than a guess.

### Pinpad PIN entry works — but only with our own token driver

Worth recording because it looks like it should, and it would have been the
elegant fix for the real usability problem: the PIN is typed over HID into
whatever window has keyboard focus, and macOS frequently does not focus its own
authorization dialog.

CCID readers can declare `bPINSupport` and collect the PIN themselves, which
would mean no text field, no keystrokes, and nothing to focus — the button press
*becomes* the PIN entry. CryptoTokenKit really does read that byte straight out
of the CCID descriptor to identify pinpad readers.

With Apple's built-in `pivtoken`: **it never sent a single
`PC_to_RDR_Secure`**, even with `bPINSupport = 0x01` declared. It ignores pinpad
entirely.

With the CryptoTokenKit driver in the [openhanko-macos](../openhanko-macos) repository, pinpad works properly —
**no dialog, nothing typed, authenticated by the button alone** — for anything
going through the Security framework. `sudo` is the exception, because
`pam_smartcard` collects a PIN before CryptoTokenKit is consulted and hands it
over pre-filled. See that repository's README.

With the CryptoTokenKit driver in the [openhanko-macos](../openhanko-macos) repository, it works. The device
receives `PC_to_RDR_Secure`, answers it from a button press, and macOS completes
the signature — 483 ms end to end, one press.

For `sudo` specifically the prompt still appears and the device still types
`000000`, so the focus problem survives there. Everywhere else it is gone.

### A trap worth knowing when trimming mbedTLS

Leaving `MBEDTLS_PEM_PARSE_C` out of the config fails in a maximally confusing
way: certificates still work, because `decode_pem_cert()` in `piv.c` does its
own base64. The card enumerates, macOS reads the identity and offers to pair —
and only signing fails, with `6f00`. The device trace is what pinned it, by
showing `6f00` where a missing button press would have given `6982`.

## Build and flash

A fresh clone needs the Pico SDK on hand — the build directory used to carry
this in its cache, so it only surfaces on a clean checkout:

```
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0
cd rp2040 && cmake -S . -B build && cmake --build build
```

Then double-tap RESET and copy `build/smart_card_rp2040.uf2` onto the volume
that appears.


Needs ESP-IDF v5.3.x (v5.3.4 verified).

```sh
source ~/esp/esp-idf/export.sh
cd simple
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem101 flash
```

Boards with two USB ports label them **COM** (UART bridge — flashing and logs)
and **USB** (native — where the smart card appears). Flash over either, but the
card only enumerates on the native port. With both connected you get logs and
the card at once; with only the native port connected, use
`./provision.py monitor` for diagnostics.

A wrong-port symptom worth recognising: `system_profiler SPSmartCardsDataType`
shows an empty `Readers:` list, and the serial port name does not change after
the firmware boots.

## Give it an identity

Two ways, and the device accepts either. `./provision.py` is stdlib-only —
no pip install, no venv.

### Compiled in — the quick way

Keys are baked into the firmware image, so a flash is all it takes:

```sh
./provision.py gen-secrets     # writes simple/main/secrets.h
cd simple && idf.py build && idf.py -p /dev/cu.usbmodemXXX flash
```

`gen-secrets` generates the RSA-2048 keypairs and writes them as correctly
escaped C string literals. `secrets.h` is gitignored. If it is absent the
firmware builds fine and waits to be provisioned over the console instead; if it
is present but still holds the `REPLACE_WITH` placeholders from
`secrets.example.h`, it is ignored with a warning.

The cost: the private keys sit in `build/` and in the flashed `.bin`, so every
device flashed from that image shares one identity.

### Provisioned over the console — per device

```sh
./provision.py provision
```

Generates keys on your Mac and pushes them over the CDC console into the
device's NVS, without reflashing. Private keys are not written to disk unless
you pass `--keep-keys DIR`.

**An NVS identity overrides a compiled-in one.** `STATUS` reports which is
active as `source=nvs` or `source=compiled`; `FACTORY_RESET` erases NVS and
falls back to the compiled keys.

### Then pair it

```sh
./provision.py ports        # find the device
./provision.py status       # what does it know?
./provision.py pair         # link it to your macOS account
```

macOS usually offers to pair on its own as soon as the card is inserted — the
"Unpaired SmartCard inserted" notification does the same thing as `pair`.

`pair` reads the identity hash back out of `sc_auth identities` and prints the
`sudo sc_auth pair` command to run (or runs it with `--run`).

Then test it:

```sh
sudo -k && sudo -v      # press the button when macOS asks for the PIN
```

`./provision.py monitor` prints device events live, which is the fastest
way to tell whether a press registered.

## How it works

```
button press
   └─> piv_note_user_presence()      opens a 10 s one-shot window for slot 9a
   └─> types "000000" + Enter        over HID, to clear the macOS PIN prompt

macOS  ──CCID/APDU──>  piv.c
   SELECT / GET DATA               certificates, CHUID, discovery object
   VERIFY                          accepted unconditionally (see below)
   GENERAL AUTHENTICATE (9a)       signs only if a press is inside the window
```

The PIN macOS collects is `000000` and is **not** a secret — it is not your
account password, and checking it would add nothing. The real gate is the
button: without a press inside the 10-second window, slot 9a refuses to sign and
macOS gets `6982`.

## The device cannot prompt you first

macOS says nothing to the card until a PIN has already been submitted.
Measured with the trace ring cleared and a prompt left on screen for ten
seconds: nothing arrived. The card is powered, then powered off again, with no
APDU in between.

```
CCID 62 IccPowerOn
CCID 63 IccPowerOff      7.5 s later, nothing asked
```

So in driverless mode the device cannot light up to say "macOS wants you" — it
has not been told. It can only acknowledge a touch after the fact. The
breathing invitation exists solely in pinpad mode, where the driver sends
`PC_to_RDR_Secure` and the device therefore knows.

An earlier version worked around this with a host-side agent that watched for
the prompt and sent an `ATTENTION` command over the console. It worked, and it
was deleted: it needed a LaunchAgent running permanently to light an LED, and
the pinpad driver made it redundant by telling the device directly.

## A warning about authentication paths

`tools/authplugin/` contains an authorization plugin that **must not be
installed** — its installer refuses to run. It once sat in the shared
`authenticate` right and blocked for up to 20 seconds per unlock attempt waiting
for a button press, which made a correct password report "incorrect" at the lock
screen and rejected Touch ID with it. A reboot was needed to get back in.

Anything placed in an authentication path must return promptly. Falling through
on failure is not sufficient protection if the failure takes twenty seconds to
arrive.

The `sudo` PAM module in `tools/pam/` does not have this property problem: it
only affects `sudo`, never the lock screen, and returns as soon as no paired card
answers.

## Security posture

Bluntly, before trusting this with anything:

- **A button proves presence, not identity.** While the trigger is the button,
  anyone who can reach the device can authenticate as you. The fingerprint sensor
  is what changes that, and it is not tested against hardware yet.
- **The PIN is theatre.** `VERIFY` accepts any PIN and opens a window. macOS
  insists on collecting one, so the presence check is the only real gate.
- **Keys are plaintext in flash.** On the RP2040 they sit outside the image and
  come straight out over SWD; on the ESP32 build `esptool.py read_flash` does the
  same. Closing this is what the RP2350's secure boot and debug lockout are for,
  and none of that is enabled yet.
- **Slot 9d is not presence-gated.** Anything that can reach the device can run
  a key agreement against it. That is deliberate — macOS unwraps the login
  keychain there right after a press the user already made — but it means the
  press authorises a session, not a single operation.
- Every attack here needs physical access to the device.

## Layout

```
rp2040/                the shipping line — builds for RP2040 and RP2350
  board_config.h       pins, AID default, timings
  main.c               cooperative loop: presence, indicator, mode switching
  piv.c                PIV applet: certificates, VERIFY, GENERAL AUTHENTICATE,
                       ECDH on slot 9D
  identity.c           generates the device's own keypair and certificate
  fingerprint.c        HLK-ZW111 over UART (EF-01) — untested against hardware
  settings.c           which AID to answer, in its own flash sector
  storage.c            the PIV identity, in flash, outside the image
  usb_ccid.c           CCID class driver over TinyUSB
  usb_hid.c            HID keyboard, for typing the PIN in driverless mode
  config_console.c     provisioning and diagnostics on CDC
  trace.c              ring buffer of CCID and APDU activity — the single most
                       useful debugging tool in this project

simple/                ESP32-S3, where this started; works, not the target
provision.py           key generation, provisioning, macOS pairing
```

The macOS driver and the site live in their own repositories:
[openhanko-macos](https://github.com/openhanko/openhanko-macos) and
openhanko-web.

## Next steps

1. **Wire a ZW111 and test `fingerprint.c`.** It is written from the protocol,
   not against a module. The search range and the LED parameters are the two
   things most likely to need adjusting.
2. **Port to RP2350** and regenerate every identity there. Keys made on an
   RP2040 come from ring-oscillator jitter and are development-only; the
   RP2350's TRNG is what makes them worth trusting.
3. **Burn OTP and lock debug**, last, on a unit you can afford to brick. This is
   the step that turns the fingerprint from a convenience into a security
   property — without it, anyone who opens the case reads the key out and never
   touches the sensor.
