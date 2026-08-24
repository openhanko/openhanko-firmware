#!/usr/bin/env python3
"""Builds the secure-boot OTP configs and signed images from your own keys.

    ./bootkeys.py primary.pem spare.pem build-rp2350/openhanko.uf2 out/

Writes into `out/`:

    signed-primary.uf2      the firmware, signed with the first key
    signed-spare.uf2        the same firmware, signed with the second
    stage1-keys.json        install both keys; signatures not yet required
    stage2-enable.json      require a valid signature to boot
    revoke-primary.json     retire the first key, leaving the spare

Nothing here talks to a device and nothing is irreversible. Loading the configs
with `picotool otp load` is the irreversible part, and that is deliberately a
separate step you take yourself.

Why a script rather than editing JSON by hand: picotool computes a boot key's
fingerprint from the PEM and always writes it as `bootkey0`, so a two-key setup
needs the second run's value moved to `bootkey1`. Doing that by copying a hash
between files invites exactly one kind of mistake, and its consequence is a
board that can never boot signed firmware again.

The private keys are read by picotool to sign, and are not copied, logged or
written anywhere by this script.
"""

import json
import os
import shutil
import subprocess
import sys

# Which key slots this uses, and which are deliberately shut.
#
# The datasheet is explicit: mark slots you do not intend to use as invalid, or
# somebody who later gets a write to OTP can install a key of their own and sign
# whatever they like. Slots 0 and 1 are ours; 2 and 3 get nailed shut.
SLOT_PRIMARY = 0
SLOT_SPARE = 1
KEY_VALID = (1 << SLOT_PRIMARY) | (1 << SLOT_SPARE)   # 0b0011
KEY_INVALID_UNUSED = 0b1100


def picotool() -> str:
    found = shutil.which("picotool")
    if found:
        return found
    # The SDK ships its own, and on macOS it is usually the one that works —
    # a Homebrew build linked against a newer libusb can fail in the USB path
    # while looking fine on files.
    guess = os.path.expanduser("~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool")
    if os.path.exists(guess):
        return guess
    sys.exit("no picotool on PATH and none in ~/.pico-sdk")


def seal(tool: str, key: str, firmware: str, out_uf2: str, out_json: str) -> list:
    """Signs `firmware` with `key`, returning the key's boot fingerprint."""
    result = subprocess.run(
        [tool, "seal", "--sign", firmware, out_uf2, key, out_json],
        capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"picotool seal failed for {key}:\n{result.stdout}{result.stderr}")
    if "signature" not in result.stdout:
        sys.exit(f"picotool did not report a signature for {key}; refusing to go on")
    with open(out_json) as f:
        return json.load(f)["bootkey0"]


def main() -> None:
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    primary, spare, firmware, outdir = sys.argv[1:5]
    os.makedirs(outdir, exist_ok=True)
    tool = picotool()

    fp_primary = seal(tool, primary, firmware,
                      os.path.join(outdir, "signed-primary.uf2"),
                      os.path.join(outdir, ".primary.json"))
    fp_spare = seal(tool, spare, firmware,
                    os.path.join(outdir, "signed-spare.uf2"),
                    os.path.join(outdir, ".spare.json"))

    # Two keys with one fingerprint means the same key was passed twice, and a
    # spare that is the primary is not a spare.
    if fp_primary == fp_spare:
        sys.exit("both keys have the same fingerprint — is that the same file twice?")

    def write(name, obj):
        path = os.path.join(outdir, name)
        with open(path, "w") as f:
            json.dump(obj, f, indent=4)
            f.write("\n")
        return path

    write("stage1-keys.json", {
        "bootkey0": fp_primary,
        "bootkey1": fp_spare,
        "boot_flags1": {"key_valid": KEY_VALID, "key_invalid": KEY_INVALID_UNUSED},
    })
    write("stage2-enable.json", {"crit1": {"secure_boot_enable": 1}})
    # Retiring the primary keeps the bits already burned: OTP only ever goes
    # 0 to 1, so a revocation is added to the mask rather than replacing it.
    write("revoke-primary.json", {
        "boot_flags1": {"key_invalid": KEY_INVALID_UNUSED | (1 << SLOT_PRIMARY)},
    })

    for tmp in (".primary.json", ".spare.json"):
        os.unlink(os.path.join(outdir, tmp))

    hexed = lambda v: "".join("%02x" % b for b in v)
    print(f"  primary fingerprint  {hexed(fp_primary)}")
    print(f"  spare fingerprint    {hexed(fp_spare)}")
    print()
    print(f"  wrote {outdir}/")
    print("    signed-primary.uf2  signed-spare.uf2")
    print("    stage1-keys.json    stage2-enable.json    revoke-primary.json")
    print()
    print("  Check those fingerprints against your own records before loading")
    print("  anything. They can be reproduced from a PEM with:")
    print("    openssl ec -in KEY.pem -pubout -outform DER | tail -c 64 | shasum -a 256")


if __name__ == "__main__":
    main()
