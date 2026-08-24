#!/usr/bin/env python3
"""Takes a fresh RP2350 board all the way to a locked, production unit.

    ./provision-board.py out/                 # rehearse, touching nothing
    ./provision-board.py out/ --commit        # actually burn

`out/` is a directory made by bootkeys.py, holding the signed firmware and the
OTP configs for your keys.

Every OTP write here is permanent. The script is built around that: it verifies
what landed before it goes on, refuses to continue when anything disagrees, and
does the steps in the one order that works.

## Why this order

The device burns its own secret into OTP the first time it generates an
identity, so it has to boot and run before page 4 is locked. Lock first and it
can never write one, and a device with no secret cannot store an identity at
all — a brick that looks like a working board until you try to pair it.

Secure boot comes after the firmware is proven to boot on that board, because
after it, an image that does not run cannot be replaced by an unsigned one.

Debug goes last of all, because it is the step that removes the ability to
diagnose the previous steps.

## What survives

SWD dies. BOOTSEL and picotool do not: they run over USB PICOBOOT, which this
deliberately leaves enabled, so a unit stays updatable with signed firmware
forever. That is the intended end state — updatable, not readable.
"""

import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from provision import Console, list_ports  # noqa: E402


class Abort(Exception):
    pass


def tool() -> str:
    import shutil
    found = shutil.which("picotool")
    if found:
        return found
    guess = os.path.expanduser("~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool")
    if os.path.exists(guess):
        return guess
    raise Abort("no picotool on PATH and none in ~/.pico-sdk")


def run(*args, check=True) -> str:
    result = subprocess.run(args, capture_output=True, text=True)
    if check and result.returncode != 0:
        raise Abort(f"{' '.join(args[:3])} failed:\n{result.stdout}{result.stderr}")
    return result.stdout + result.stderr


def in_bootsel(pt: str) -> bool:
    return subprocess.run([pt, "info"], capture_output=True).returncode == 0


def need_bootsel(pt: str) -> None:
    """Gets the board into BOOTSEL, asking for hands if necessary.

    There is no way to do this from here. picotool can reboot a device it can
    already talk to, but the firmware deliberately exposes no picotool vendor
    interface — the USB interfaces belong to the smart card — so a running board
    cannot be sent to the bootloader by software. Earlier this waited for a
    BOOTSEL it had no means of causing, which reads as a hang.
    """
    if in_bootsel(pt):
        return
    print()
    print("  >>> put the board in BOOTSEL: double-tap RESET, or hold BOOT while replugging")
    print("  >>> waiting...", end="", flush=True)
    if not wait_for(lambda: in_bootsel(pt), 300):
        raise Abort("gave up waiting for BOOTSEL")
    print(" got it")
    print()


def wait_for(predicate, seconds: int) -> bool:
    deadline = time.time() + seconds
    while time.time() < deadline:
        if predicate():
            return True
        time.sleep(1)
    return False


def otp_rows(pt: str, tmp: str) -> list:
    """The whole OTP as a list of 4096 row values."""
    import struct
    run(pt, "otp", "dump", "--output", tmp)
    with open(tmp, "rb") as f:
        raw = f.read()
    stride = len(raw) // 4096
    return [struct.unpack_from("<I", raw, i * stride)[0] for i in range(4096)]


def key_at(rows: list, start: int) -> str:
    return "".join("%02x" % b for i in range(16)
                   for b in (rows[start + i] & 0xff, (rows[start + i] >> 8) & 0xff))


def say(step: str, detail: str = "") -> None:
    print(f"  {step:<34} {detail}")


def burn(pt: str, config: str, commit: bool) -> None:
    if not commit:
        say("would burn", os.path.basename(config))
        return
    run(pt, "otp", "load", config)


def main() -> None:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    outdir = sys.argv[1]
    commit = "--commit" in sys.argv
    pt = tool()
    tmp = os.path.join(outdir, ".otpdump.bin")

    signed = os.path.join(outdir, "signed-primary.uf2")
    stage1 = os.path.join(outdir, "stage1-keys.json")
    stage2 = os.path.join(outdir, "stage2-enable.json")
    for f in (signed, stage1, stage2):
        if not os.path.exists(f):
            raise Abort(f"{f} missing — run bootkeys.py first")

    expect = json.load(open(stage1))
    want0 = "".join("%02x" % b for b in expect["bootkey0"])
    want1 = "".join("%02x" % b for b in expect["bootkey1"])

    print()
    print("  OpenHanko board provisioning" + ("" if commit else "   [REHEARSAL — nothing will be burned]"))
    print()

    # ---- 1. the board must be fresh ---------------------------------------
    if not in_bootsel(pt):
        raise Abort("put the board in BOOTSEL first (hold BOOT while plugging in)")
    rows = otp_rows(pt, tmp)
    flags1, crit1 = rows[0x04b], rows[0x040]
    lock4 = rows[0xf89]
    done_keys = bool(flags1 & 0xf)
    done_tap = bool(flags1 & (1 << 19))
    done_lock = bool(lock4 & 0xffffff)
    done_secure = bool(crit1 & 1)
    done_debug = bool(crit1 & 4)

    # Resumable by inspection rather than by a flag. A run that stops partway
    # leaves a board in a real state, and the only safe way to continue is to
    # read what that state is — a --resume flag would be the operator asserting
    # it, which is the thing that goes wrong at 2am.
    if done_debug:
        raise Abort("debug is already disabled — this board is finished")
    if done_keys:
        say("resuming", f"BOOT_FLAGS1=0x{flags1:06x} CRIT1=0x{crit1:06x}")
    else:
        say("board is fresh", "no keys, no flags, no locks")

    # ---- 2. firmware first, so there is something to boot ------------------
    #
    # Deliberately without -x. That flag starts the application, and everything
    # in the next two steps needs the board still sitting in the bootloader —
    # picotool answers "rebooting" to an OTP write aimed at a device that has
    # just been told to run.
    if commit:
        run(pt, "load", signed)
    say("flashed signed firmware", os.path.basename(signed))
    if not done_tap and "double_tap" not in json.dumps(expect.get("boot_flags1", {})):
        raise Abort("stage1-keys.json has no double_tap — it predates the bootrom "
                    "recovery. Re-run bootkeys.py before provisioning, or the board "
                    "loses its way back in the moment secure boot is enabled.")

    # ---- 3. keys and the bootrom's recovery, before anything requires them --
    if done_keys and done_tap:
        say("keys already burned", "skipping")
    else:
        burn(pt, stage1, commit)
        say("burned boot keys + double-tap")

    if commit:
        # Still in the bootloader from step 2, so the readback needs no detour.
        rows = otp_rows(pt, tmp)
        got0, got1 = key_at(rows, 0x080), key_at(rows, 0x090)
        if got0 != want0:
            raise Abort(f"bootkey0 readback mismatch\n  want {want0}\n  got  {got0}")
        if got1 != want1:
            raise Abort(f"bootkey1 readback mismatch\n  want {want1}\n  got  {got1}")
        say("verified both keys", "byte-for-byte")
    else:
        say("would verify both keys", "against stage1-keys.json")

    # ---- 4. let the device make its own secret -----------------------------
    # This has to happen before page 4 is locked. The firmware provisions the
    # secret when it first generates an identity, and cannot do it afterwards.
    if commit:
        run(pt, "reboot", "-a", check=False)
        if not wait_for(lambda: bool(list_ports()), 45):
            raise Abort("the board did not come up after flashing — stop here, it is "
                        "still recoverable while secure boot is off")
        time.sleep(3)
        with Console(list_ports()[0]) as console:
            status = console.send("STATUS", echo=False)[-1]
        if "otp=set" not in status:
            raise Abort(f"the device did not provision its secret:\n  {status}")
        if "keys=loaded" not in status:
            raise Abort(f"the device has no identity:\n  {status}")
        name = status.split('name="')[1].rstrip('"') if 'name="' in status else "?"
        say("device made its own secret", f"identity {name}")
    else:
        say("would boot and wait", "for otp=set and keys=loaded")

    # ---- 5. seal the secret away -------------------------------------------
    need_bootsel(pt) if commit else None
    lock = os.path.join(outdir, "lock-otp-secret.json")
    if not os.path.exists(lock):
        with open(lock, "w") as f:
            json.dump({"page4_lock1": {"lock_s": 1, "lock_ns": 3, "lock_bl": 3}}, f, indent=4)
            f.write("\n")
    if done_lock:
        say("secret's page already locked", "skipping")
    else:
        burn(pt, lock, commit)
        say("locked the secret's page", "secure read-only, bootloader shut out")

    # ---- 6. require signatures ---------------------------------------------
    if done_secure:
        say("secure boot already on", "skipping")
    else:
        burn(pt, stage2, commit)
        say("enabled secure boot")

    if commit:
        run(pt, "load", "-x", signed, check=False)
        if not wait_for(lambda: bool(list_ports()), 45):
            run(pt, "reboot", "-a", check=False)
            if not wait_for(lambda: bool(list_ports()), 30):
                raise Abort("the signed firmware does not boot under secure boot. "
                            "Debug is still on — diagnose before going further.")
        say("signed firmware boots", "under secure boot")

    # ---- 7. and finally, no more looking inside -----------------------------
    dbg = os.path.join(outdir, "disable-debug.json")
    if not os.path.exists(dbg):
        with open(dbg, "w") as f:
            json.dump({"crit1": {"debug_disable": 1, "secure_debug_disable": 1}}, f, indent=4)
            f.write("\n")
    if commit:
        need_bootsel(pt)
    burn(pt, dbg, commit)
    say("disabled debug", "SWD is gone; BOOTSEL and picotool remain")

    if commit and os.path.exists(tmp):
        os.unlink(tmp)

    print()
    if commit:
        print("  Provisioned. The board updates with signed firmware and reveals nothing.")
    else:
        print("  Rehearsal only. Re-run with --commit to burn.")
    print()


if __name__ == "__main__":
    try:
        main()
    except Abort as e:
        sys.stdout.flush()
        print(f"\n  STOPPED: {e}\n", file=sys.stderr)
        sys.exit(1)
