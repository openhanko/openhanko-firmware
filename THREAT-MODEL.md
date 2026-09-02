# OpenHanko — threat model

Scoped per use case, because one blanket claim would be wrong in both directions:
the device defends a compromised host well, and defends a stolen device barely at
all.

**Everything under [Today](#5-today-what-actually-gates-what) describes shipped
behaviour, verified against the source**, and
[section 7](#7-what-each-defence-buys) marks each measure as in force or not.
Anything not marked in force must not be claimed.

Last checked against `src/` in September 2026.

---

## 1. What the device is

A USB PIV smart card with a presence check. It signs with a P-256 key it
generated on-device, and macOS authenticates against that key.

The consequence that drives this whole document: **PIV is a general-purpose
standard.** A device that macOS can log in with is a device `ssh-keygen -D` can
read an identity from. We cannot expose one without the other — slot 9A is the
same slot in both cases, and the 9A certificate must exist or macOS pairing
fails entirely. Telling users not to do it does not stop them.

## 2. Assets

| asset | where it lives | if it leaks |
| --- | --- | --- |
| slot 9A private key | device flash, AES-256-GCM | attacker authenticates as the user, anywhere the key is trusted |
| slot 9D private key | device flash, AES-256-GCM | attacker unwraps anything wrapped to it, incl. the login keychain |
| device secret | OTP page 4, chaffed, locked | both keys above, since it is what wraps them |
| fingerprint templates | sensor module flash | biometric data; readable by whoever holds the module |
| PIN | **nowhere — there is no real PIN** | n/a today |

The PIN row is the important one. Every secret the device relies on is *inside
the device*, so possession of the device is possession of everything. A PIN is
the only candidate secret that would not be, which is why it is the hinge of
section 6.

## 3. Attackers

| | capability | defensible |
| --- | --- | --- |
| **A. Compromised host** | full control of the Mac; cannot touch the device | **yes** — the primary defended threat |
| **B. Offline extraction** | has the device powered off; can desolder, read flash, use lab equipment | **yes** — flash yields ciphertext, and the key to it is in locked OTP |
| **C. Physical possession, live** | has a working device; can open the case, cut and drive the sensor harness, glitch power | **no** — mitigations raise cost only |

## 4. Device states

Posture varies more across these than across anything else, so a claim is
meaningless without saying which one it describes.

| | state | debug port | key at rest | glitch errata |
| ---: | --- | --- | --- | --- |
| 1 | **RP2350 A2** | lockable | plaintext | E16, E20, E24 all open |
| 2 | **RP2350 A4**, no lockdown | open until fused | plaintext | fixed in silicon |
| 3 | **RP2350 A4** + secure boot + SWD fused | closed | plaintext | fixed in silicon |
| 4 | **RP2350 A4** + above + key wrapped to OTP | closed | ciphertext | fixed in silicon |
| 5 | **RP2350 A4** + above + PIN in the KDF | closed | ciphertext, underivable without the PIN | fixed in silicon |

Only the last row makes a stolen device inert, and row 5 needs a PIN that does
not exist yet. `STATUS` reports `chip=` and `otp=`, which between them say which
row a device is on; nothing else reports either reliably.

**A provisioned unit is row 4** — A4 silicon, secure boot on, SWD fused, key
material wrapped to a chaffed OTP secret. The bootloader reads that page as zeros
and `picotool` is refused, while the signed firmware on the same die reads it and
decrypts its identity.

The RP2354A's in-package flash raises the cost again. There is no separate chip
to lift off, so the offline attack starts at decapsulation rather than hot air —
and what it recovers is ciphertext.

## 5. Today: what actually gates what

Verified in `src/`:

| operation | what it requires | what that costs an attacker holding the device |
| --- | --- | --- |
| `VERIFY` | any bytes — the PIN is discarded (`(void)data;`), always `9000`, 60 s window | nothing |
| `GENERAL AUTHENTICATE` **slot 9D** (ECDH) | a fingerprint match inside a 60 s session window, not consumed on use | an enrolled finger |
| `GENERAL AUTHENTICATE` **slot 9A** (sign) | a fingerprint match inside a 10 s window | an enrolled finger, which the user presents |
| enrolling a finger | a button press *while a matching finger is on the sensor*; on a device with no template yet, the press alone | an enrolled finger and the device in hand |
| `CONFIG_UNLOCK`, then `BOOTLOADER` | a button press, then a 120 s window | one press, and the bootloader still demands a signed image |
| `AID_MODE` | a button press of its own, not the `CONFIG_UNLOCK` window | one press — and it changes which driver binds, not what signs |
| `IDLE_LIGHT` | the `CONFIG_UNLOCK` window | nothing; it gates nothing and reveals nothing |
| factory reset | button held through power-up; **no host path at all** | — |

There is no PIN retry counter and no lockout, because there is no PIN to count
against. `VERIFY` returns `9000` rather than the `63CX` retries-remaining a
standard PIV card returns.

**The console cannot reach a key.** In a shipped image no command signs,
generates or loads key material, enrols a finger, erases a template or burns a
fuse; what is left reads state, reboots, or writes one of the two settings above.
That matters because the console shares a cable with the card — anything it can
do, a compromised host can do — and it is the constraint that decides what may
ever be added to it.

Two module bring-up commands sit outside that. `FINGERPRINT_REG` writes the
module's registers and `FINGERPRINT_GETKEY` asks it for a key pair, which the
manual says clears every enrolled template. Both exist only behind
`-DFINGERPRINT_LAB_TOOLS=ON`, which is off by default and must stay off in
anything shipped.

**Slot 9D is gated against a window of its own.** A match opens 60 s there that
use does not consume, rather than the 10 s slot 9A spends per signature. macOS
unwraps the login keychain on 9D immediately after the 9A signature that logged
the user in, so checking 9A's window would refuse the unwrap that always follows
a successful login.

### What does hold today

- **The private key never existed off-device.** Not "on the generate-on-device
  path" — there is no other path. Nothing in the firmware accepts a key from
  outside, so no provisioning machine ever held one: there is no copy to leak and
  no vendor to trust.
- **Signing is RFC 6979 deterministic**, so a weak RNG cannot leak the key
  through a repeated or predictable nonce.
- **Factory reset has no host-reachable path.** Malware cannot destroy a user's
  credentials, whatever it sends.
- **A fingerprint is required per 9A signature window**, so a compromised host
  cannot sign in the background while the device sits in a dock.

That last line is the real product: **against attacker A, the device works.**

## 6. Per use case

### Local unlock — macOS login, `sudo`, authorisation prompts

**Honest today.** A compromised host cannot sign without a fingerprint. A stolen
device is worth having only together with the user's Mac, and the exposure ends
at that Mac.

Presence is a fingerprint match, verified on hardware. It proves a finger
enrolled on this device is present — which is a claim about *who*, bounded by
what the sensor can distinguish and by the fact that the link carrying the answer
is unauthenticated.

### Remote-capable credentials — SSH, PGP, CA-issued certificates, code signing

**Not defended. A stolen device is a full compromise.** The attacker does not
need the user's Mac, or the user's network, or anything else — the credential is
remote-usable by definition, and the device will sign for whoever can put an
enrolled finger — or a forged sensor answer — in front of it.

This is the case the device is not built for and cannot refuse. It should be
stated in the product documentation in these words, not softened.

### Physical possession

**Not defended, though swapping the sensor does not work.** The device records
its module's `PS_GetChipSN` and refuses everything if it later meets a different
one, so the cheap attack — fit a module you control — is closed.

What remains is the link itself, which is plain UART with no authenticated mode
and a 4-byte password that is not merely sent in clear but readable from the
parameter page. An attacker who opens the case can drive the harness and forge a match
response, and secure boot, SWD lockout and OTP protection all keep working
correctly — they are not in that path.

**It cannot be fixed on this module.** The manual documents a safety instruction
set that would turn the link into a challenge-response, gated behind an
encryption level in register 7. Three independent readings say the set is not
implemented on this module: `PS_GetCiphertext` answers exactly as an opcode that
does not exist does; writing register 7 is accepted but leaves `Secur Level`
reading 0, so the level never changes; and `PS_GetKeyt` returns success without
clearing enrolled templates, which is its first documented act and the one it
cannot skip.

A PIN closes the same attack without depending on the module, and is the only
measure here that does.

See [the sensor link](README.md#the-sensor-link-cannot-be-authenticated).

## 7. What each defence buys

Ordered by what they actually close, not by effort.

| defence | closes | does not close |
| --- | --- | --- |
| RP2350 **A4** silicon | E16 glitch-to-debug, E20 unsigned boot, E24 laser fault | anything above |
| Secure boot + SWD fused *(done)* | reading the key over the debug port; flashing firmware that prints the secret | desoldering the flash |
| **Key wrapped to an OTP secret** *(done)* | attacker B — a flash reader yields ciphertext | attacker C, who has the die and can ask the firmware to sign |
| OTP **chaffing** *(done)* | the IOActive PVC/FIB antifuse read — the one hardware attack **A4 does not fix** | a lab willing to spend more than the chaffing costs |
| Two boot keys with revocation *(done)* | a leaked or lost signing key ending the fleet | a leak nobody notices |
| Sensor binding via `PS_GetChipSN` *(done)* | swapping the module for another — the serial is per-die, confirmed across modules | an emulator replaying the expected serial |
| `TouchOut` correlation *(done)*, staged protocol, timing bounds | replaying one packet on RX | reading the published protocol and driving two lines |
| **PIN mixed into the wrapping KDF** | **attacker C** — a stolen device is inert, forging a match unwraps nothing | someone who watches the user type the PIN |
| ~~Authenticated sensor link~~ | would have closed the forged match outright | **unavailable** — the safety instruction set is not implemented on this module, so there is no encryption level to raise |

Two notes that are easy to get wrong:

**A PIN checked only in firmware buys almost nothing.** Secure boot verifies what
executes without encrypting what is stored, so a PIN the firmware merely checks
is bypassed by reading the flash. Only a PIN that is an *input to the KDF*
survives that — and with the OTP secret already in the KDF, a PIN would be the
second input rather than the first.

**A real PIN means the device must stop typing it.** In driverless mode the
device types the PIN over HID; a PIN the device knows is not a secret from
someone holding the device. So enabling it is a UX change — plug in, type once
per session, then touch — not a flag flip.

## 8. Gaps, ordered

1. **No real PIN.** The *only* remaining defence against attacker C, now that an
   authenticated sensor link is ruled out on this module: if the link cannot be
   trusted, the answer it carries has to stop being sufficient on its own. The
   at-rest work it depended on is done, so this is buildable rather than blocked,
   and it depends on nothing the sensor does or does not support.
2. **The firmware is the oracle.** It can read the OTP secret — that is the
   design — so a bug that leaks it costs everything the lockdown bought. A
   standing constraint, not a task: never add anything that returns it.
3. **A touch authorises a window, not an operation, and nothing rate-limits
   it.** Slot 9A accepts any signature for 10 s after a match and slot 9D for
   60 s, the latter not consumed on use, with no cap on how many arrive inside
   either. In driverless mode the window is structural — the card is told
   nothing until a PIN arrives, so the touch has to come first.

## 9. Claims we may and may not make

**May, of a provisioned unit:**

- The private key is generated on the device, never transmitted, never copied to
  a host. There is no firmware path that could do otherwise.
- It is stored encrypted, under a key the flash does not contain.
- The debug port is fused shut and only firmware signed with our key will run.
- A compromised host cannot sign without a fingerprint.
- Credentials cannot be erased by any host command.
- The device refuses to work if its fingerprint sensor is exchanged.
- The firmware and the protocol it speaks are published.

Each of those was verified on hardware rather than reasoned about.

**May not:**

- ~~"Tamper-proof."~~ Passive voltage contrast with an ion beam reads antifuse
  cells directly. Chaffing raises the price; it does not close it.
- ~~"Safe for SSH keys."~~ Still the case with the least defence — see §6.
- ~~"The fingerprint proves it is you."~~ It proves an enrolled finger is on the
  bound sensor. The link carrying that answer is unauthenticated, so someone who
  opens the case can assert it without a finger.
- ~~"A stolen device is safe."~~ It is not, and §6 says how far that goes.

**Must state plainly:** a stolen device still authorises whatever a forged sensor
answer authorises. Binding stopped the sensor being *swapped*; nothing yet stops
it being *driven*. For local unlock the exposure is bounded by needing the user's
Mac. For anything remote-capable it is not bounded at all, and no amount of the
work above changes that — only a PIN would.
