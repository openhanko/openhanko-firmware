# OpenHanko — threat model

Scoped per use case, because one blanket claim would be wrong in both directions:
the device defends a compromised host well, and defends a stolen device barely at
all.

**Everything under [Today](#5-today-what-actually-gates-what) describes shipped
behaviour, verified against the source.** Everything under
[Planned](#7-what-each-planned-defence-buys) does not exist yet and must not be
claimed. That separation is the point of the document: earlier drafts of our own
README claimed a locked debug port and a `sudo` limitation that were both
untrue, in opposite directions.

Last checked against `src/` after bring-up on RP2350 A4 with a real ZW111.

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
| slot 9A private key | device flash, plaintext | attacker authenticates as the user, anywhere the key is trusted |
| slot 9D private key | device flash, plaintext | attacker unwraps anything wrapped to it, incl. the login keychain |
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
| **B. Offline extraction** | has the device powered off; can desolder, read flash, use lab equipment | **not today**; achievable with encryption at rest |
| **C. Physical possession, live** | has a working device; can open the case, cut and drive the sensor harness, glitch power | **no** — mitigations raise cost only |

## 4. Device states

Posture varies more across these than across anything else, so a claim is
meaningless without saying which one it describes.

| state | debug port | key at rest | glitch errata |
| --- | --- | --- | --- |
| **RP2350 A2** | lockable | plaintext in external QSPI | E16, E20, E24 all open |
| **RP2350 A4**, no lockdown | open until fused | plaintext in external QSPI | fixed in silicon |
| **RP2350 A4** + secure boot + SWD fused | closed | plaintext in external QSPI | fixed in silicon |
| **RP2350 A4** + above + key wrapped to OTP | closed | ciphertext | fixed in silicon |
| **RP2350 A4** + above + PIN in the KDF | closed | ciphertext, underivable without the PIN | fixed in silicon |

Only the last row makes a stolen device inert. Read `chip=` from `STATUS` to
find out which silicon is in front of you; nothing else reports it reliably.

**Everything so far is row 2** — A4 silicon with nothing fused. The RP2354A's
in-package flash raises the cost of the offline attack even at that row: there is
no separate chip to lift off, so reading it means decapsulation rather than hot
air.

## 5. Today: what actually gates what

Verified in `src/piv.c`:

| operation | what it requires | what that costs an attacker holding the device |
| --- | --- | --- |
| `VERIFY` | any bytes — the PIN is discarded (`(void)data;`), always `9000`, 60 s window | nothing |
| `GENERAL AUTHENTICATE` **slot 9D** (ECDH) | a button press inside a 60 s session window, not consumed on use | one press |
| `GENERAL AUTHENTICATE` **slot 9A** (sign) | a button press inside a 10 s window | one press, which they perform |
| `PAIRING_MODE` | a press; then 9A signs freely for 120 s | one press |
| console config commands | a press, then a 120 s window | one press |
| factory reset | button held through power-up; **no host path at all** | — |

There is no PIN retry counter and no lockout, because there is no PIN to count
against. `VERIFY` returns `9000` rather than the `63CX` retries-remaining a
standard PIV card returns.

Slot 9D used to be the sharpest edge: ungated entirely, so a compromised host
could run key agreement against it silently and at will. It is now gated on a
press, but against a *separate 60 s window* that signing does not consume —
because macOS unwraps the login keychain there immediately after the 9A
signature that logged the user in, and checking 9A's own window would refuse the
unwrap that always follows a successful login.

Verified on hardware: press → 9A `9000` → 9D `9000` 1.2 s later, no prompts.

### What does hold today

- **The private key never existed off-device** on the generate-on-device path.
  No provisioning machine ever held it, so there is no copy to leak and no
  vendor to trust.
- **Signing is RFC 6979 deterministic**, so a weak RNG cannot leak the key
  through a repeated or predictable nonce.
- **Factory reset has no host-reachable path.** Malware cannot destroy a user's
  credentials, whatever it sends.
- **A press is required per 9A signature window**, so a compromised host cannot
  sign in the background while the device sits in a dock.

That last line is the real product: **against attacker A, the device works.**

## 6. Per use case

### Local unlock — macOS login, `sudo`, authorisation prompts

**Honest today.** A compromised host cannot sign without a press. A stolen device
is worth having only together with the user's Mac, and the exposure ends at that
Mac.

Presence is a fingerprint match, verified on hardware. It proves a finger
enrolled on this device is present — which is a claim about *who*, bounded by
what the sensor can distinguish and by the fact that the link carrying the answer
is unauthenticated.

### Remote-capable credentials — SSH, PGP, CA-issued certificates, code signing

**Not defended. A stolen device is a full compromise.** The attacker does not
need the user's Mac, or the user's network, or anything else — the credential is
remote-usable by definition, and the device will sign for whoever presses it.

This is the case the device is not built for and cannot refuse. It should be
stated in the product documentation in these words, not softened.

### Physical possession

**Not defended, and not fixable at the sensor.** The ZW111 link is plain UART
with no authenticated mode and a 4-byte password sent in clear on every
power-up. An attacker who opens the case can drive the harness and forge a match
response, and secure boot, SWD lockout and OTP protection all keep working
correctly — they are not in that path.

See [the sensor link](README.md#the-sensor-link-cannot-be-authenticated).

## 7. What each planned defence buys

Ordered by what they actually close, not by effort.

| defence | closes | does not close |
| --- | --- | --- |
| RP2350 **A4** silicon | E16 glitch-to-debug, E20 unsigned boot, E24 laser fault | anything above |
| Secure boot + SWD fused | reading the key off a live device through the debug port | desoldering the flash |
| **Key wrapped to an OTP secret** | attacker B — flash reader yields ciphertext | attacker C, who has the die and can ask the firmware to sign |
| OTP **chaffing** (complementary bit pairs) | the IOActive PVC/FIB antifuse read — the one hardware attack **A4 does not fix** | — |
| Sensor binding via `PS_ReadINFpage` | swapping in a stock module — **only if the serial is per-unit, which is unconfirmed** | an emulator replaying the expected serial |
| `TouchOut` correlation *(done)*, staged protocol, timing bounds | replaying one packet on RX | reading the published protocol and driving two lines |
| **PIN mixed into the wrapping KDF** | **attacker C** — a stolen device is inert, forging a match unwraps nothing | someone who watches the user type the PIN |

Two notes that are easy to get wrong:

**A PIN checked only in firmware buys almost nothing.** RP2350-Zero keeps data in
a *separate QSPI flash package*, and secure boot verifies what executes without
encrypting what is stored. Desolder, read, ignore the firmware. Only a PIN that
is an *input to the KDF* survives that.

**A real PIN means the device must stop typing it.** In driverless mode the
device types the PIN over HID; a PIN the device knows is not a secret from
someone holding the device. So enabling it is a UX change — plug in, type once
per session, then touch — not a flag flip.

## 8. Gaps, ordered

1. **No real PIN.** Blocks the only defence against attacker C. Needs the at-rest
   work first, which needs RP2350.
2. **Key material is plaintext at rest**, and on RP2350-Zero that flash is a
   separate package.
3. **Module binding not built.** `PS_GetChipSN` (`0x34`) returns a 32-byte
   per-die serial and is implemented, but nothing binds to it yet, and one
   module is not enough to confirm the value differs between units.
4. **No rate limiting** on signature operations.
5. **A touch authorises a window, not an operation.** Slot 9A accepts any
   signature for 10 s after a match and slot 9D for 60 s, the latter not
   consumed on use. In driverless mode this is structural — the card is told
   nothing until a PIN arrives, so the touch has to come first.

## 9. Claims we may and may not make

**May:**

- The private key is generated on the device and is never transmitted, copied or
  backed up — no provisioning machine ever held it. That is a statement about
  normal operation, not about an attacker with the device: see §4, where every
  shipped state still has it extractable.
- A compromised host cannot sign without a physical press.
- Credentials cannot be erased by any host command.
- Firmware, hardware and protocol are published.

**May not, today:**

- ~~"The debug port is locked."~~ Not enabled on any unit.
- ~~"Tamper-resistant."~~ No secure element, and SWD is not fused on any unit yet.
- ~~"Safe for SSH keys."~~ Precisely the case with no defence.
- ~~"The fingerprint proves it is you."~~ It proves an enrolled finger is on the
  sensor, which is not the same claim: the link carrying that answer is
  unauthenticated, so someone who opens the case can assert it without a finger.

**Must state plainly:** a stolen device is a full compromise of every credential
on it. For local unlock that is bounded by needing the user's Mac. For anything
remote-capable it is not bounded at all.
