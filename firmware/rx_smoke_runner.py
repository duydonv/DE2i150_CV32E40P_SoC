#!/usr/bin/env python3
"""Send framed UART RX smoke-test payloads to the DE2i-150 firmware."""

from __future__ import annotations

import argparse
import struct
import time

try:
    import serial
except ImportError as exc:
    raise SystemExit("rx_smoke_runner.py requires pyserial: python3 -m pip install pyserial") from exc


SYNC = b"\x55\xaa"
CMD_ECHO = 0x01
FNV_OFFSET = 2166136261
FNV_PRIME = 16777619


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


def make_payload(length: int, seq: int) -> bytes:
    return bytes(((idx * 37 + seq * 11 + 0x5A) & 0xFF) for idx in range(length))


def make_frame(cmd: int, payload: bytes) -> bytes:
    checksum = frame_checksum(cmd, payload)
    return SYNC + bytes([cmd]) + struct.pack("<H", len(payload)) + payload + struct.pack("<I", checksum)


def parse_lengths(text: str) -> list[int]:
    lengths = [int(item, 0) for item in text.split(",") if item.strip()]
    for length in lengths:
        if length < 0 or length > 512:
            raise argparse.ArgumentTypeError("lengths must be in 0..512")
    return lengths


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
        if text.startswith("OK ") or text.startswith("ERR "):
            return text
    raise TimeoutError("timed out waiting for OK/ERR response; recent lines: " + repr(lines[-5:]))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("port", help="serial port, for example /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--lengths", type=parse_lengths, default=parse_lengths("0,1,16,64,255,512"))
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--timeout", type=float, default=5.0)
    parser.add_argument("--inter-frame-delay", type=float, default=0.05)
    args = parser.parse_args()

    with serial.Serial(args.port, args.baud, timeout=0.2) as port:
        time.sleep(0.2)
        port.reset_input_buffer()

        seq = 0
        for _ in range(args.repeat):
            for length in args.lengths:
                seq += 1
                payload = make_payload(length, seq)
                port.write(make_frame(CMD_ECHO, payload))
                port.flush()
                response = read_response(port, args.timeout)
                print(response)
                if not response.startswith("OK "):
                    raise SystemExit(1)
                time.sleep(args.inter_frame_delay)


if __name__ == "__main__":
    main()
