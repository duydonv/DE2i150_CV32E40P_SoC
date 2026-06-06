#!/usr/bin/env python3
"""Send runtime inputs to the TFLM tiny UART inference firmware."""

from __future__ import annotations

import argparse
import re
import struct
import time

try:
    import serial
except ImportError as exc:
    raise SystemExit(
        "tflm_tiny_uart_runner.py requires pyserial: python3 -m pip install pyserial"
    ) from exc


SYNC = b"\x55\xaa"
CMD_PING = 0x10
CMD_INFER = 0x11
FNV_OFFSET = 2166136261
FNV_PRIME = 16777619

INPUT_DIM = 64
OUTPUT_DIM = 4
SAMPLE_COUNT = 8

OK_RE = re.compile(
    r"^OK seq=(?P<seq>\d+) cmd=0x(?P<cmd>[0-9a-fA-F]+) len=(?P<len>\d+)"
    r"(?: pass=(?P<pass>yes|no) ref_cls=(?P<ref_cls>\d+) opt_cls=(?P<opt_cls>\d+)"
    r" mismatches=(?P<mismatches>\d+).*)?$"
)


def checksum_update(state: int, value: int) -> int:
    state ^= value & 0xFF
    state = (state * FNV_PRIME) & 0xFFFFFFFF
    return state


def frame_checksum(cmd: int, payload: bytes) -> int:
    state = FNV_OFFSET
    length = len(payload)
    for value in (cmd, length & 0xFF, (length >> 8) & 0xFF):
        state = checksum_update(state, value)
    for value in payload:
        state = checksum_update(state, value)
    return state


def make_frame(cmd: int, payload: bytes) -> bytes:
    checksum = frame_checksum(cmd, payload)
    return SYNC + bytes([cmd]) + struct.pack("<H", len(payload)) + payload + struct.pack("<I", checksum)


def is_quadrant(label: int, row: int, col: int) -> bool:
    top = row < 4
    left = col < 4
    return (
        (label == 0 and top and left)
        or (label == 1 and top and not left)
        or (label == 2 and not top and left)
        or (label == 3 and not top and not left)
    )


def clamp_i8(value: int) -> int:
    return max(-64, min(63, value))


def make_quadrant_sample(label: int, variant: int) -> bytes:
    values: list[int] = []
    for row in range(8):
        for col in range(8):
            value = 48 if is_quadrant(label, row, col) else -24
            if variant == 1:
                if is_quadrant(label, row, col) and ((row * 3 + col * 5 + label) & 3) == 0:
                    value += 8
                elif (row + col + label) % 7 == 0:
                    value -= 4
            values.append(clamp_i8(value) & 0xFF)
    return bytes(values)


def quadrant_samples() -> list[tuple[bytes, int]]:
    samples: list[tuple[bytes, int]] = []
    for cls in range(OUTPUT_DIM):
        for variant in range(2):
            samples.append((make_quadrant_sample(cls, variant), cls))
    return samples


def pattern_sample(seq: int) -> bytes:
    return bytes(((idx * 37 + seq * 11 + 0x5A) & 0xFF) for idx in range(INPUT_DIM))


def read_response(port: serial.Serial, timeout_s: float) -> str:
    deadline = time.monotonic() + timeout_s
    lines: list[str] = []
    while time.monotonic() < deadline:
        line = port.readline()
        if not line:
            continue
        text = line.decode("ascii", errors="replace").strip()
        if text:
            lines.append(text)
        if text.startswith("OK ") or text.startswith("ERR ") or text.startswith("FATAL "):
            return text
    raise TimeoutError("timed out waiting for response; recent lines: " + repr(lines[-5:]))


def send_frame(port: serial.Serial, cmd: int, payload: bytes, timeout_s: float) -> str:
    port.write(make_frame(cmd, payload))
    port.flush()
    return read_response(port, timeout_s)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("port", help="serial port, for example /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=10.0)
    parser.add_argument("--inter-frame-delay", type=float, default=0.05)
    parser.add_argument("--mode", choices=("quadrants", "pattern"), default="quadrants")
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.2) as port:
        time.sleep(0.2)
        port.reset_input_buffer()

        ping = send_frame(port, CMD_PING, b"", args.timeout)
        print(ping)
        if not ping.startswith("OK "):
            raise SystemExit(1)

        seq = 0
        failures = 0
        for _ in range(args.repeat):
            if args.mode == "quadrants":
                samples = quadrant_samples()
            else:
                samples = [(pattern_sample(seq + idx), -1) for idx in range(SAMPLE_COUNT)]

            for payload, expected in samples:
                seq += 1
                response = send_frame(port, CMD_INFER, payload, args.timeout)
                print(response)
                match = OK_RE.match(response)
                ok = match is not None and match.group("pass") == "yes"
                if ok and expected >= 0:
                    ref_cls = int(match.group("ref_cls"))
                    opt_cls = int(match.group("opt_cls"))
                    ok = ref_cls == expected and opt_cls == expected
                if not ok:
                    failures += 1
                time.sleep(args.inter_frame_delay)

        if failures != 0:
            raise SystemExit(f"{failures} TFLM UART inference frame(s) failed")


if __name__ == "__main__":
    main()
