#!/usr/bin/env python3
"""Host-side setup for the fingerprint-gated PIV smart card.

Deliberately dependency-free: it talks to the device's CDC console with plain
POSIX file I/O so there is nothing to install first.

    ./provision.py ports
    ./provision.py status
    ./provision.py pair
    ./provision.py monitor

There is no command here that puts a key on the device. It generates its own at
first boot and no private key has ever left it; the two routes that used to
upload one from this machine — a staged push over the console, and baking PEMs
into the firmware image as secrets.h — are gone, along with the console commands
that received them.
"""

from __future__ import annotations

import argparse
import getpass
import glob
import os
import re
import select
import subprocess
import sys
import termios
import time
import tty



class Failure(Exception):
    pass


def say(message: str = "") -> None:
    print(message, flush=True)


# --------------------------------------------------------------------------
# CDC console
# --------------------------------------------------------------------------


class Console:
    """A line-oriented connection to the device's USB CDC interface."""

    def __init__(self, port: str) -> None:
        self.port = port
        try:
            self.fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as error:
            raise Failure(f"cannot open {port}: {error}") from error
        try:
            tty.setraw(self.fd)
        except termios.error:
            # Some CDC stacks reject termios calls; raw byte I/O still works.
            pass
        self.buffer = b""
        # Give macOS a moment to finish opening the CDC endpoint.
        time.sleep(0.4)
        self.drain()

        # A previous client killed mid-write can leave half a command in the
        # firmware's line buffer, which would otherwise be prepended to ours.
        # Send a bare newline to terminate it, then discard whatever that
        # produces — an orphaned fragment comes back ERR UNKNOWN_COMMAND, and
        # reading that as the reply to the *next* command is its own bug.
        os.write(self.fd, b"\n")
        time.sleep(0.2)
        self.drain()

    def close(self) -> None:
        os.close(self.fd)

    def __enter__(self) -> "Console":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def drain(self) -> None:
        while select.select([self.fd], [], [], 0)[0]:
            try:
                if not os.read(self.fd, 4096):
                    break
            except OSError:
                break
        self.buffer = b""

    def read_line(self, timeout: float) -> str | None:
        deadline = time.monotonic() + timeout
        while True:
            if b"\n" in self.buffer:
                raw, self.buffer = self.buffer.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip()
                if line:
                    return line
                continue
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            if not select.select([self.fd], [], [], min(remaining, 0.25))[0]:
                continue
            try:
                chunk = os.read(self.fd, 4096)
            except OSError:
                return None
            if chunk:
                self.buffer += chunk

    def _drain(self, window: float) -> list[str]:
        """Lines that arrive after a terminator, until the device goes quiet."""
        trailing: list[str] = []
        while True:
            line = self.read_line(window)
            if line is None:
                return trailing
            trailing.append(line)

    def send(self, command: str, timeout: float = 20.0, echo: bool = True) -> list[str]:
        """Send one command and collect lines until the OK/ERR terminator."""
        os.write(self.fd, (command + "\n").encode("ascii"))
        lines: list[str] = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.read_line(deadline - time.monotonic())
            if line is None:
                break
            lines.append(line)
            if echo:
                if line == "PROMPT PRESS":
                    say("  → press the button on the device now")
                elif line.startswith(("EVENT ", "PROMPT ")):
                    say(f"  · {line}")
            if line.startswith(("OK", "ERR")) or line == "PONG":
                # Most commands put the terminator last. FINGERPRINT_INFO_RAW
                # puts it first and follows it with the page, so stopping here
                # threw away everything that made the command worth running.
                # Draining afterwards tolerates both orders rather than encoding
                # which commands are which.
                lines.extend(self._drain(0.35))
                break
        if not lines:
            raise Failure(
                f"{self.port} did not answer '{command}'. Is the firmware running, "
                "and is this the device's CDC port? Try ./provision.py ports."
            )
        if lines[-1].startswith("ERR"):
            raise Failure(f"device rejected '{command}': {lines[-1]}")
        return lines


# --------------------------------------------------------------------------
# Ports
# --------------------------------------------------------------------------


def list_ports() -> list[str]:
    return sorted(glob.glob("/dev/cu.usbmodem*"))


def pick_port(explicit: str | None) -> str:
    if explicit:
        return explicit
    ports = list_ports()
    if not ports:
        raise Failure(
            "no /dev/cu.usbmodem* device found. Is the board plugged in and "
            "running the firmware rather than sitting in the bootloader?"
        )
    if len(ports) > 1:
        say(f"several ports found, using {ports[0]}")
        say("pass --port to choose a different one: " + ", ".join(ports))
    return ports[0]


# --------------------------------------------------------------------------
# Key generation
# --------------------------------------------------------------------------


def command_ports(_: argparse.Namespace) -> None:
    ports = list_ports()
    if not ports:
        say("no /dev/cu.usbmodem* devices found")
        return
    for port in ports:
        say(port)


def command_status(args: argparse.Namespace) -> None:
    with Console(pick_port(args.port)) as console:
        for line in console.send("STATUS", timeout=5):
            say(line)


def command_console(args: argparse.Namespace) -> None:
    with Console(pick_port(args.port)) as console:
        for line in console.send(args.text, timeout=args.timeout):
            say(line)


def command_monitor(args: argparse.Namespace) -> None:
    port = pick_port(args.port)
    say(f"watching {port}; touch the sensor to see events. Ctrl-C to stop.")
    with Console(port) as console:
        try:
            while True:
                line = console.read_line(1.0)
                if line:
                    say(line)
        except KeyboardInterrupt:
            say("")


def command_pair(args: argparse.Namespace) -> None:
    port = pick_port(args.port)

    say("Looking for the card's identity in macOS.")
    identities: list[tuple[str, str]] = []
    for attempt in range(20):
        result = subprocess.run(["sc_auth", "identities"], capture_output=True, text=True)
        identities = []
        for line in result.stdout.splitlines():
            match = re.search(r"\b([0-9A-Fa-f]{40})\b\s*(.*)", line)
            if match:
                identities.append((match.group(1), match.group(2).strip()))
        if identities:
            break
        if attempt == 0:
            say("  waiting for macOS to read the card...")
        time.sleep(1.0)

    if not identities:
        raise Failure(
            "macOS does not see any smart-card identity. Check "
            "'system_profiler SPSmartCardsDataType' and confirm provisioning succeeded."
        )

    preferred = [item for item in identities if "authentication" in item[1].lower()]
    chosen = (preferred or identities)[0]
    say(f"  found: {chosen[0]}  {chosen[1]}")

    pair_command = ["sc_auth", "pair", "-u", getpass.getuser(), "-h", chosen[0]]
    say("")
    if args.run:
        say("Running: sudo " + " ".join(pair_command))
        result = subprocess.run(["sudo", *pair_command])
        if result.returncode != 0:
            raise Failure("sc_auth pair failed")
        say("Paired.")
    else:
        say("Run this to finish pairing (it needs sudo, so run it yourself):")
        say("")
        say("  sudo " + " ".join(pair_command))
        say("")
        say("Then test with:  sudo -k && sudo -v")


def main() -> int:
    # --port is accepted on either side of the subcommand.
    port_option = argparse.ArgumentParser(add_help=False)
    port_option.add_argument("--port", help="CDC device path, e.g. /dev/cu.usbmodem101")

    parser = argparse.ArgumentParser(description=__doc__, parents=[port_option],
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("ports", help="list candidate serial ports").set_defaults(
        handler=command_ports)
    subparsers.add_parser("status", help="ask the device what it knows",
                          parents=[port_option]).set_defaults(handler=command_status)
    subparsers.add_parser("monitor", help="print device events until Ctrl-C",
                          parents=[port_option]).set_defaults(handler=command_monitor)

    pair = subparsers.add_parser("pair", help="link the card to your macOS account",
                                 parents=[port_option])
    pair.add_argument("--run", action="store_true", help="run sc_auth pair via sudo")
    pair.set_defaults(handler=command_pair)

    console = subparsers.add_parser("console", help="send one raw console command",
                                    parents=[port_option])
    console.add_argument("text")
    console.add_argument("--timeout", type=float, default=20.0)
    console.set_defaults(handler=command_console)

    args = parser.parse_args()
    try:
        args.handler(args)
    except Failure as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
