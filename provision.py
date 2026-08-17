#!/usr/bin/env python3
"""Host-side setup for the button-gated PIV smart card.

Deliberately dependency-free: it talks to the device's CDC console with plain
POSIX file I/O so there is nothing to install before the proof of concept runs.

    ./provision.py ports
    ./provision.py status
    ./provision.py gen-secrets     # bake keys into the firmware image
    ./provision.py provision       # or store them over the console
    ./provision.py pair
    ./provision.py monitor
"""

from __future__ import annotations

import argparse
import base64
import getpass
import glob
import os
import re
import select
import subprocess
import sys
import tempfile
import termios
import time
import tty

OPENSSL = "/usr/bin/openssl"
CHUNK = 480
REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SECRETS_PATH = os.path.join(REPO_ROOT, "firmware", "simple", "main", "secrets.h")
SLOTS = (
    ("cert9a", "cert_9a", "authentication certificate"),
    ("key9a", "key_9a", "authentication private key"),
    ("cert9d", "cert_9d", "key-management certificate"),
    ("key9d", "key_9d", "key-management private key"),
)


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
                break
        if not lines:
            raise Failure(
                f"{self.port} did not answer '{command}'. Is the firmware running, "
                "and is this the device's CDC port rather than its serial-JTAG port?"
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
            "no /dev/cu.usbmodem* device found. Plug the board into the USB port "
            "wired to the S3's native USB pins, not the UART-bridge port."
        )
    if len(ports) > 1:
        say(f"several ports found, using {ports[0]}")
        say("pass --port to choose a different one: " + ", ".join(ports))
    return ports[0]


# --------------------------------------------------------------------------
# Key generation
# --------------------------------------------------------------------------


def generate_identity(common_name: str, directory: str, algorithm: str = "ec") -> dict[str, str]:
    """Generates the 9a/9d keypairs and self-signed certificates.

    ec  -> NIST P-256, PIV algorithm 0x11. Signs in ~200 ms on an RP2040.
    rsa -> RSA-2048, PIV algorithm 0x07. ~3 s on an RP2040, tens of ms on an
           ESP32-S3, which has a big-integer accelerator.
    """
    if not os.path.exists(OPENSSL):
        raise Failure("no openssl at /usr/bin/openssl")
    if algorithm == "ec":
        # ec_param_enc:named_curve is not optional. LibreSSL defaults to writing
        # the curve out explicitly — field type, prime, generator, the lot — and
        # mbedTLS rejects that with MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE unless
        # MBEDTLS_PK_PARSE_EC_EXTENDED is enabled. Named curves are also what
        # real PIV cards carry, and the key drops from 377 to 135 bytes.
        keyspec = ["-newkey", "ec",
                   "-pkeyopt", "ec_paramgen_curve:prime256v1",
                   "-pkeyopt", "ec_param_enc:named_curve"]
    elif algorithm == "rsa":
        keyspec = ["-newkey", "rsa:2048"]
    else:
        raise Failure(f"unknown algorithm {algorithm!r}")

    bundle: dict[str, str] = {}
    for slot, label in (("9a", "authentication"), ("9d", "key management")):
        key_path = os.path.join(directory, f"key-{slot}.pem")
        cert_path = os.path.join(directory, f"cert-{slot}.pem")
        subject = f"/CN={common_name} {label}/"
        result = subprocess.run(
            [OPENSSL, "req", *keyspec, "-nodes",
             "-keyout", key_path, "-x509", "-sha256", "-days", "3650",
             "-out", cert_path, "-subj", subject],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise Failure(f"openssl failed for slot {slot}: {result.stderr.strip()}")
        with open(key_path, encoding="utf-8") as handle:
            bundle[f"key_{slot}"] = handle.read()
        with open(cert_path, encoding="utf-8") as handle:
            bundle[f"cert_{slot}"] = handle.read()
    return bundle


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------


def c_string(pem: str) -> str:
    """Render a PEM as a sequence of C string literals, one per line."""
    literals = []
    for line in pem.splitlines():
        escaped = line.replace("\\", "\\\\").replace('"', '\\"')
        literals.append(f'"{escaped}\\n"')
    return "\n".join(literals)


def command_gen_secrets(args: argparse.Namespace) -> None:
    destination = os.path.abspath(args.output or SECRETS_PATH)
    if os.path.exists(destination) and not args.force:
        # The build seeds a placeholder secrets.h, which is not worth protecting.
        with open(destination, encoding="utf-8") as handle:
            placeholder = "REPLACE_WITH" in handle.read()
        if not placeholder:
            raise Failure(f"{destination} holds real keys; pass --force to replace it")

    name = args.name or f"smart-card-poc {getpass.getuser()}"
    label = "P-256" if args.algorithm == "ec" else "RSA-2048"
    say(f"Generating a {label} identity for '{name}'.")
    with tempfile.TemporaryDirectory(prefix="smart-card-piv-") as directory:
        bundle = generate_identity(name, directory, args.algorithm)

    header = [
        "// Generated by ./provision.py gen-secrets. Do not commit.",
        "//",
        "// These private keys are compiled into the firmware image, so every",
        "// device flashed from that image shares this one identity.",
        "",
        "#pragma once",
        "",
    ]
    fields = [
        ("PIV_CERT_9A_PEM", "cert_9a"),
        ("PIV_PRIVATE_KEY_9A_PEM", "key_9a"),
        ("PIV_CERT_9D_PEM", "cert_9d"),
        ("PIV_PRIVATE_KEY_9D_PEM", "key_9d"),
    ]
    body = []
    for symbol, bundle_key in fields:
        body.append(f"static const char {symbol}[] =")
        body.append(c_string(bundle[bundle_key]) + ";")
        body.append("")

    os.makedirs(os.path.dirname(destination), exist_ok=True)
    with open(destination, "w", encoding="utf-8") as handle:
        handle.write("\n".join(header + body))
    os.chmod(destination, 0o600)

    say(f"  wrote {destination}")
    say("")
    say("Now build and flash:")
    say("  cd simple && idf.py build && idf.py -p <port> flash")
    say("")
    say("Then pair it:  ./provision.py pair")


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
    say(f"watching {port}; press the device button to see events. Ctrl-C to stop.")
    with Console(port) as console:
        try:
            while True:
                line = console.read_line(1.0)
                if line:
                    say(line)
        except KeyboardInterrupt:
            say("")


def command_provision(args: argparse.Namespace) -> None:
    port = pick_port(args.port)
    name = args.name or f"smart-card-poc {getpass.getuser()}"

    with Console(port) as console:
        say(f"device: {port}")
        for line in console.send("STATUS", timeout=5):
            say(f"  {line}")

        say("")
        say("Unlocking configuration. If the device already holds keys, it will")
        say("ask for a button press to prove you have physical access.")
        console.send("CONFIG_UNLOCK", timeout=25)

        say("")
        label = "P-256" if args.algorithm == "ec" else "RSA-2048"
        say(f"Generating a fresh {label} identity for '{name}'.")
        with tempfile.TemporaryDirectory(prefix="smart-card-piv-") as directory:
            bundle = generate_identity(name, directory, args.algorithm)
            if args.keep_keys:
                target = os.path.abspath(args.keep_keys)
                os.makedirs(target, exist_ok=True)
                for filename in os.listdir(directory):
                    with open(os.path.join(directory, filename), encoding="utf-8") as src:
                        content = src.read()
                    destination = os.path.join(target, filename)
                    with open(destination, "w", encoding="utf-8") as dst:
                        dst.write(content)
                    os.chmod(destination, 0o600)
                say(f"  copy of the private keys written to {target}")

            say("Storing it on the device.")
            console.send("PROVISION_BEGIN", timeout=5)
            for device_name, bundle_key, label in SLOTS:
                say(f"  · {label}")
                encoded = base64.b64encode(bundle[bundle_key].encode("utf-8")).decode("ascii")
                for offset in range(0, len(encoded), CHUNK):
                    console.send(
                        f"PROVISION_CHUNK {device_name} {encoded[offset:offset + CHUNK]}",
                        timeout=5, echo=False,
                    )
            console.send("PROVISION_COMMIT", timeout=10)

        say("")
        say("Re-enumerating USB so macOS re-reads the card.")
        try:
            console.send("USB_RECONNECT", timeout=3)
        except Failure:
            # The USB detach usually beats the acknowledgment back to the host.
            pass

    time.sleep(2.0)
    say("")
    say("Done. Next:")
    say("  system_profiler SPSmartCardsDataType   # macOS should list the card")
    say("  ./provision.py pair              # link it to your account")


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

    say("")
    say("Enabling pairing mode so the handshake does not need a press per signature.")
    with Console(port) as console:
        console.send("PAIRING_MODE", timeout=25)

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

    secrets = subparsers.add_parser(
        "gen-secrets", help="generate keys and write simple/main/secrets.h")
    secrets.add_argument("--name", help="common name to put in the certificates")
    secrets.add_argument("--output", metavar="PATH", help="write somewhere other than main/secrets.h")
    secrets.add_argument("--force", action="store_true", help="overwrite an existing secrets.h")
    secrets.add_argument("--algorithm", choices=("ec", "rsa"), default="ec",
                         help="ec = P-256 (default, fast everywhere); "
                              "rsa = RSA-2048 (the ESP32 build only supports this)")
    secrets.set_defaults(handler=command_gen_secrets)

    provision = subparsers.add_parser("provision", help="generate and store a PIV identity",
                                      parents=[port_option])
    provision.add_argument("--name", help="common name to put in the certificates")
    provision.add_argument("--keep-keys", metavar="DIR",
                           help="also save the generated private keys to DIR")
    provision.add_argument("--algorithm", choices=("ec", "rsa"), default="ec",
                           help="ec = P-256 (default, fast everywhere); "
                                "rsa = RSA-2048 (the ESP32 build only supports this)")
    provision.set_defaults(handler=command_provision)

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
