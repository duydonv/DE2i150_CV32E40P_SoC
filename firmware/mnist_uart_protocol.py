"""Reusable MNIST UART protocol and input conversion helpers."""

from __future__ import annotations

import csv
import re
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO


SYNC = b"\x55\xaa"
CMD_PING = 0x10
CMD_INFER = 0x11
FNV_OFFSET = 2166136261
FNV_PRIME = 16777619

INPUT_DIM = 784
IMAGE_SIDE = 28
OUTPUT_DIM = 10

FIRMWARE_DIR = Path(__file__).resolve().parent
DEFAULT_VECTOR_HEADER = FIRMWARE_DIR / "mnist_fc" / "mnist_fc_test_vectors.h"
DEFAULT_IMAGE_DIR = FIRMWARE_DIR / "mnist_fc" / "test_images_pgm"


@dataclass(frozen=True)
class MnistSample:
    name: str
    payload: bytes
    label: int | None = None
    expected_class: int | None = None
    expected_scores: tuple[int, ...] | None = None
    source: str | None = None


def checksum_update(state: int, value: int) -> int:
    state ^= value & 0xFF
    return (state * FNV_PRIME) & 0xFFFFFFFF


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
    return (
        SYNC
        + bytes([cmd])
        + struct.pack("<H", len(payload))
        + payload
        + struct.pack("<I", checksum)
    )


def read_response(port, timeout_s: float) -> str:
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


def send_frame(port, cmd: int, payload: bytes, timeout_s: float) -> str:
    port.write(make_frame(cmd, payload))
    port.flush()
    return read_response(port, timeout_s)


def find_initializer(text: str, name: str) -> str:
    anchor = text.find(name)
    if anchor < 0:
        raise ValueError(f"cannot find {name}")
    equals = text.find("=", anchor)
    start = text.find("{", equals)
    if equals < 0 or start < 0:
        raise ValueError(f"cannot find initializer for {name}")

    depth = 0
    for pos in range(start, len(text)):
        char = text[pos]
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return text[start : pos + 1]
    raise ValueError(f"unterminated initializer for {name}")


def parse_ints(text: str, name: str) -> list[int]:
    body = find_initializer(text, name)
    return [int(match.group(0)) for match in re.finditer(r"-?\d+", body)]


def u8_to_i8(value: int) -> int:
    value &= 0xFF
    return value - 256 if value >= 128 else value


def pixels_to_payload(pixels: bytes | bytearray | list[int] | tuple[int, ...]) -> bytes:
    if len(pixels) != INPUT_DIM:
        raise ValueError(f"expected {INPUT_DIM} pixels, got {len(pixels)}")
    return bytes(((int(pixel) & 0xFF) - 128) & 0xFF for pixel in pixels)


def load_test_vectors(header: Path = DEFAULT_VECTOR_HEADER) -> list[MnistSample]:
    text = header.read_text()
    input_values = parse_ints(text, "mnist_fc_test_inputs")
    labels = parse_ints(text, "mnist_fc_test_labels")
    expected_classes = parse_ints(text, "mnist_fc_test_expected_classes")
    expected_output_bytes = parse_ints(text, "mnist_fc_test_expected_output_bytes")

    if len(input_values) % INPUT_DIM != 0:
        raise ValueError(f"input vector count is not a multiple of {INPUT_DIM}")
    vector_count = len(input_values) // INPUT_DIM
    if len(labels) != vector_count or len(expected_classes) != vector_count:
        raise ValueError("label/class count does not match input vector count")
    if len(expected_output_bytes) != vector_count * OUTPUT_DIM:
        raise ValueError("expected score count does not match vector count")

    samples: list[MnistSample] = []
    for index in range(vector_count):
        start = index * INPUT_DIM
        out_start = index * OUTPUT_DIM
        raw_values = input_values[start : start + INPUT_DIM]
        expected_scores = tuple(
            u8_to_i8(value)
            for value in expected_output_bytes[out_start : out_start + OUTPUT_DIM]
        )
        samples.append(
            MnistSample(
                name=f"vector={index}",
                payload=bytes(value & 0xFF for value in raw_values),
                label=labels[index],
                expected_class=expected_classes[index],
                expected_scores=expected_scores,
                source=str(header),
            )
        )
    return samples


def read_pgm_token(stream: BinaryIO) -> bytes:
    token = bytearray()
    while True:
        char = stream.read(1)
        if char == b"":
            raise ValueError("unexpected EOF while reading PGM header")
        if char == b"#":
            stream.readline()
            continue
        if char.isspace():
            if token:
                return bytes(token)
            continue
        token.extend(char)


def read_pgm_pixels(path: Path) -> bytes:
    with path.open("rb") as stream:
        magic = read_pgm_token(stream)
        if magic != b"P5":
            raise ValueError(f"{path} is not a binary PGM/P5 image")
        width = int(read_pgm_token(stream))
        height = int(read_pgm_token(stream))
        max_value = int(read_pgm_token(stream))
        if width != IMAGE_SIDE or height != IMAGE_SIDE:
            raise ValueError(f"{path} has shape {width}x{height}, expected 28x28")
        if max_value <= 0 or max_value > 255:
            raise ValueError(f"{path} has unsupported max value {max_value}")
        pixels = stream.read(INPUT_DIM)
        if len(pixels) != INPUT_DIM:
            raise ValueError(f"{path} has {len(pixels)} pixels, expected {INPUT_DIM}")
        if stream.read(1) != b"":
            raise ValueError(f"{path} has trailing bytes after {INPUT_DIM} pixels")
    if max_value != 255:
        pixels = bytes(round(pixel * 255 / max_value) for pixel in pixels)
    return pixels


def parse_label_from_name(path: Path) -> int | None:
    match = re.search(r"(?:^|_)label([0-9])(?:\D|$)", path.stem)
    if match is None:
        return None
    return int(match.group(1))


def load_labels_csv(path: Path) -> dict[str, int]:
    labels: dict[str, int] = {}
    with path.open(newline="") as f:
        reader = csv.DictReader(f)
        if "file" not in (reader.fieldnames or []) or "label" not in (reader.fieldnames or []):
            raise ValueError(f"{path} must contain file,label columns")
        for row in reader:
            labels[row["file"]] = int(row["label"])
    return labels


def load_pgm_sample(path: Path, labels: dict[str, int] | None = None) -> MnistSample:
    label = None
    if labels is not None:
        label = labels.get(path.name)
    if label is None:
        label = parse_label_from_name(path)

    return MnistSample(
        name=path.name,
        payload=pixels_to_payload(read_pgm_pixels(path)),
        label=label,
        source=str(path),
    )


def load_pgm_dir(directory: Path, labels_csv: Path | None = None) -> list[MnistSample]:
    if labels_csv is None and (directory / "labels.csv").exists():
        labels_csv = directory / "labels.csv"
    labels = load_labels_csv(labels_csv) if labels_csv is not None else None

    paths = sorted(directory.glob("*.pgm"))
    if not paths:
        raise ValueError(f"no .pgm images found in {directory}")
    return [load_pgm_sample(path, labels) for path in paths]


def parse_response_fields(line: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def parse_scores_csv(value: str) -> tuple[int, ...]:
    if value == "":
        return ()
    return tuple(int(item) for item in value.split(","))


def int_field(fields: dict[str, str], name: str) -> int:
    return int(fields[name], 0)


def validate_infer_response(
    line: str,
    sample: MnistSample,
    *,
    require_label_match: bool = False,
) -> tuple[bool, dict[str, object]]:
    if not line.startswith("OK "):
        return False, {"name": sample.name, "error": "non-ok-response", "line": line}

    fields = parse_response_fields(line)
    try:
        ref_scores = parse_scores_csv(fields["ref_scores"])
        opt_scores = parse_scores_csv(fields["opt_scores"])
        result: dict[str, object] = {
            "name": sample.name,
            "source": sample.source,
            "label": sample.label,
            "expected_class": sample.expected_class,
            "pass": fields.get("pass") == "yes",
            "ref_cls": int_field(fields, "ref_cls"),
            "opt_cls": int_field(fields, "opt_cls"),
            "mismatches": int_field(fields, "mismatches"),
            "ref_cycles": int_field(fields, "ref_cycles"),
            "opt_cycles": int_field(fields, "opt_cycles"),
            "ref_scores": ref_scores,
            "opt_scores": opt_scores,
            "line": line,
        }
    except (KeyError, ValueError) as exc:
        return False, {"name": sample.name, "error": f"parse-error: {exc}", "line": line}

    label_match = (
        sample.label is not None
        and result["ref_cls"] == sample.label
        and result["opt_cls"] == sample.label
    )
    expected_match = (
        sample.expected_class is not None
        and result["ref_cls"] == sample.expected_class
        and result["opt_cls"] == sample.expected_class
    )
    result["label_match"] = label_match
    result["expected_match"] = expected_match

    ok = (
        result["pass"] is True
        and result["ref_cls"] == result["opt_cls"]
        and result["mismatches"] == 0
        and ref_scores == opt_scores
    )
    if sample.expected_scores is not None:
        ok = ok and expected_match
        ok = ok and ref_scores == sample.expected_scores
        ok = ok and opt_scores == sample.expected_scores
    if require_label_match:
        ok = ok and label_match

    result["host_pass"] = ok
    return ok, result
