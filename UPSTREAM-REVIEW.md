# Review of `ZimengXiong/tinyTouch` → `firmware/tiny_touch_smartcard`

Read at commit fetched 2026-08-15. The PIV/CCID core is sound and the APDU
handling is more complete than it first looks — command chaining, `GET
RESPONSE` chunking, and the CCID descriptor are all correct, and the whole
thing builds clean against ESP-IDF v5.3.4. What follows is what I changed, what
I deliberately did not change, and why.

## Fixed in `firmware/simple`

### 1. `tud_task()` is called from two tasks at once

`app_main` runs:

```c
while (true) { usb_ccid_task(); vTaskDelay(pdMS_TO_TICKS(1)); }   // -> tud_task()
```

But `tinyusb_driver_install()` already starts a task of its own:

```c
// managed_components/espressif__esp_tinyusb/tinyusb_task.c:87
while (1) { tud_task(); }
```

and that task cannot be turned off — `tinyusb_task_check_config()` rejects a
task size or priority of 0. So two tasks pump the same USB event queue and
class-driver state concurrently. TinyUSB does not support that.

It evidently works in practice, probably because the second pump usually finds
an empty queue, but it is a genuine race. `firmware/simple` deletes the loop and
lets the driver's own task own the stack.

### 2. 1.7 kB certificate buffer on the APDU dispatch stack

`handle_get_data()` declared `uint8_t object[1700]` as an automatic in two
branches. APDUs are dispatched from the TinyUSB task's stack, and there is no
capacity check against the assembled object length. Now a single `static`
scratch buffer with an explicit bounds check, and the two duplicated branches
are one `respond_certificate()` helper.

### 3. `wait_hid_ready()` spins forever

```c
static void wait_hid_ready(void) { while (!tud_hid_ready()) vTaskDelay(...); }
```

A host that never polls the HID endpoint wedges the presence task permanently.
Now bounded at 500 ms per report, and a failure is reported rather than hung on.

### 4. Latent TLV length bug in `handle_general_authenticate`

```c
off += encode_len(response + off, 1 + (sig_len >= 0x80 ? 3 : 1) + sig_len);
```

`encode_len()` emits **two** bytes for lengths `0x80..0xff`, not three. For a
signature in that size range the outer `0x7c` length would be one byte too
large and the host would reject the response. Not reachable today — RSA-2048
always produces exactly 256 bytes — so this is latent, not live. It matters the
moment anyone adds a smaller key. Now uses `encoded_len_size()`, the function
that already existed for exactly this.

### 5. Every sub-10 ms delay is silently a no-op

`CONFIG_FREERTOS_HZ=100`, so one tick is 10 ms and `pdMS_TO_TICKS()` truncates:

```c
pdMS_TO_TICKS(7)  == 0     // between HID key press and release reports
pdMS_TO_TICKS(5)  == 0     // HID ready poll
pdMS_TO_TICKS(2)  == 0     // CDC write retry backoff
```

So the 7 ms pacing `send_key()` appears to place between keystrokes does not
exist — the reports go out back to back, and only `tud_hid_ready()` keeps them
from being dropped. Same for the CDC console's retry loop, which becomes a busy
spin bounded only by its 2-second deadline.

`firmware/simple` sets `CONFIG_FREERTOS_HZ=1000` so these delays mean what they
say.

### 6. The RNG feeding mbedTLS is not a true random source

`piv_rng()` hands `esp_fill_random()` to mbedTLS for every signing operation.
From `esp_random.h`:

> If Wi-Fi or Bluetooth are enabled, this function returns true random numbers.
> **In other situations**, if true random numbers are required then consult the
> ESP-IDF Programming Guide "Random Number Generation" section for necessary
> prerequisites.

Neither firmware enables Wi-Fi or Bluetooth, so the hardware RNG is seeded once
at boot and is thereafter a PRNG.

The consequence today is limited rather than fatal: PKCS#1 v1.5 padding is
deterministic, so the RNG is used for blinding, and weak blinding costs
side-channel resistance rather than leaking the key directly.

It becomes fatal the moment anyone switches the slots to ECDSA — which is the
obvious move for a port to a chip without an RSA accelerator, and the reason
this is worth fixing now rather than later. A predictable ECDSA nonce recovers
the private key from a single signature.

`firmware/simple` calls `bootloader_random_enable()` at startup, which runs the
SAR ADC entropy source. Safe here because nothing else touches the ADC or the RF
subsystem — but anything that later adds BLE must call
`bootloader_random_disable()` before initialising the radio.

### 7. `secrets.h` is dead code, and the README points at it

`main/secrets.example.h` exists, `.gitignore` excludes `main/secrets.h`, and the
README's "blue pill" section tells you to paste four PEMs into it. But **nothing
in the smartcard firmware includes `secrets.h`** — `grep -rn secrets.h` in that
directory returns nothing. Keys are read exclusively from NVS, written by the
`PROVISION_*` console commands that `tinytouch` drives.

Following the README as written gets you a device with no identity and no error
message explaining why.

**Implemented here rather than dropped**, so the documented workflow actually
works. `tools/provision.py gen-secrets` generates the keys and writes the
header, since hand-pasting a PEM into a C string literal is its own small
misery. A `secrets.h` still holding the `REPLACE_WITH` placeholders is ignored
rather than producing a card that enumerates but cannot sign. An NVS-provisioned
identity takes precedence; `STATUS` reports which source won.

The include is deliberately **unconditional**, with `main/CMakeLists.txt`
seeding the file from `secrets.example.h` when absent. The obvious
`__has_include` version is a trap: it records no dependency, so creating
`secrets.h` after a build that lacked it leaves a stale object file and silently
flashes a firmware with no keys. That cost an hour here before the binary was
checked with `strings`.

## Deliberately not changed

You said security guarantees can be lower for the PoC, so these stay as
upstream has them. They are listed because they should be conscious choices.

### 8. `VERIFY` accepts any PIN

`handle_verify()` ignores the PIN bytes entirely (`(void)data;`) and opens a
60-second window. This is intentional and documented upstream — macOS insists on
collecting a PIN, and the design moves authorization to the presence check
instead. Worth stating plainly: **the PIN is not a second factor, it is
theatre.** The button is the only gate.

### 9. Slot 9d has no presence gate at all

The presence check is scoped to one slot:

```c
if (apdu[3] == 0x9a) { /* require fingerprint / button */ }
```

Slot 9d (key management) needs only the always-succeeding `VERIFY` from #8. So
any process that can reach the USB device can use the 9d private key without
anyone touching the device.

It is sharper than it sounds. When the challenge is exactly 256 bytes the code
takes the raw path:

```c
rc = mbedtls_rsa_private(rsa, piv_rng, NULL, challenge, sig);
```

That is an unauthenticated **raw RSA private-key oracle** on attacker-chosen
256-byte blocks — no hashing, no padding check, no presence requirement.

Left as-is because macOS uses 9d for key management and gating it risks breaking
flows I cannot test without hardware in front of me. To close it, in
`firmware/simple/main/piv.c`:

```c
-  if (apdu[3] == 0x9a) {
+  if (apdu[3] == 0x9a || apdu[3] == 0x9d) {
```

### 10. Private keys are plaintext in NVS

Without secure boot and flash encryption enabled, `esptool.py read_flash` pulls
both PIV private keys straight out of the device. Upstream says this too. It is
the single biggest reason this is a proof of concept and not a security key.

### 11. The dummy PIN goes to whatever has keyboard focus

Every press types `000000` and Enter on the HID interface, unconditionally. Press
it with a text editor focused and that is what you get. Inherent to delivering
the PIN over HID; upstream has the same behaviour.

One available refinement, not implemented: only type the PIN when the card has
seen PIV APDU traffic in the last few seconds, which implies macOS is actually
prompting. I left it out because it can only make the PoC fail in ways that are
annoying to diagnose.

## Smaller notes

- **`sdkconfig` is committed** upstream (2000+ lines), so it churns with every
  IDF version and can silently override `sdkconfig.defaults`. Here only
  `sdkconfig.defaults` is tracked.
- **`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` plus TinyUSB.** The ESP32-S3 has one
  internal USB PHY, routed to *either* USB-Serial/JTAG or USB-OTG. Once TinyUSB
  claims it, `ESP_LOG` output over serial-JTAG stops arriving. On a board with
  only the native USB port, the CDC console is your only diagnostic channel —
  which is what `tools/provision.py monitor` is for.
- **`ccid_open()` hardcodes `sizeof(tusb_desc_interface_t) + 54`** to skip the
  CCID class descriptor. Correct against the descriptor as written, and silently
  wrong if anyone edits it. Kept, since the descriptor is not meant to change.
- **`fingerprint_authorize_poll_once()` polls the sensor over UART every
  250 ms**, waking the CPU constantly. Irrelevant while wired, but it will matter
  for the battery-powered BLE variant.

## What the simplification removed

| | upstream | `firmware/simple` |
| --- | ---: | ---: |
| `fingerprint.c` — ZW101 UART protocol | 368 | — |
| `touch_pin_hid.c` — HID password crypto | 248 | — |
| `device_config.c` — PIV/HID dual mode | 81 | — |
| `button.c` — debounced GPIO | — | 83 |
| `usb_hid.c` — PIN typing | — | 71 |
| **total `main/`** | **2057** | **1500** |

The 697 lines carrying the fingerprint sensor, the AES/HMAC password channel to
the macOS helper, and the two-mode configuration state machine collapse to 154
lines of "is the button down". The PIV and CCID layers are substantially intact —
that part of upstream is the valuable part.
