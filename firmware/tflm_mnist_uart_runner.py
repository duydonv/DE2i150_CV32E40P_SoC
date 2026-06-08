#!/usr/bin/env python3
"""Send MNIST inputs to the UART inference firmware."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

try:
    import serial
except ImportError as exc:
    raise SystemExit(
        "tflm_mnist_uart_runner.py requires pyserial: python3 -m pip install pyserial"
    ) from exc

from mnist_uart_protocol import (
    CMD_INFER,
    CMD_PING,
    DEFAULT_IMAGE_DIR,
    DEFAULT_VECTOR_HEADER,
    MnistSample,
    load_labels_csv,
    load_pgm_dir,
    load_pgm_sample,
    load_test_vectors,
    send_frame,
    validate_infer_response,
)


def select_vectors(samples: list[MnistSample], indices: list[int] | None) -> list[MnistSample]:
    if not indices:
        return samples
    by_index = {index: sample for index, sample in enumerate(samples)}
    selected: list[MnistSample] = []
    for index in indices:
        if index not in by_index:
            raise ValueError(f"vector index {index} is outside 0..{len(samples) - 1}")
        selected.append(by_index[index])
    return selected


def load_image_samples(
    image_paths: list[Path] | None,
    images_dir: Path | None,
    labels_csv: Path | None,
    limit: int | None,
) -> list[MnistSample]:
    samples: list[MnistSample] = []

    if images_dir is not None:
        samples.extend(load_pgm_dir(images_dir, labels_csv))

    if image_paths:
        labels = load_labels_csv(labels_csv) if labels_csv is not None else None
        samples.extend(load_pgm_sample(path, labels) for path in image_paths)

    if limit is not None:
        samples = samples[:limit]
    if not samples:
        raise ValueError("no image samples selected")
    return samples


def json_ready(value):
    if isinstance(value, tuple):
        return list(value)
    if isinstance(value, dict):
        return {key: json_ready(item) for key, item in value.items()}
    if isinstance(value, list):
        return [json_ready(item) for item in value]
    return value


def format_sample_result(ok: bool, sample: MnistSample, result: dict[str, object]) -> str:
    status = "PASS" if ok else "FAIL"
    if sample.expected_scores is not None:
        expected = sample.expected_class if sample.expected_class is not None else "?"
        return (
            f"{status} {sample.name} label={sample.label} expected={expected} "
            f"{result.get('line', '')}"
        )

    label_text = "?" if sample.label is None else str(sample.label)
    label_match = result.get("label_match")
    label_status = "unknown"
    if sample.label is not None:
        label_status = "yes" if label_match else "no"
    return (
        f"{status} image={sample.name} label={label_text} "
        f"label_match={label_status} {result.get('line', '')}"
    )


def print_summary(results: list[dict[str, object]], failures: int) -> None:
    passed = len(results) - failures
    print(f"summary: {passed}/{len(results)} samples pass")
    if not results:
        return

    ref_cycles = [int(result["ref_cycles"]) for result in results if "ref_cycles" in result]
    opt_cycles = [int(result["opt_cycles"]) for result in results if "opt_cycles" in result]
    if ref_cycles and opt_cycles and sum(opt_cycles) != 0:
        print(
            "ref cycles: "
            f"{min(ref_cycles)}..{max(ref_cycles)} avg={sum(ref_cycles) // len(ref_cycles)}"
        )
        print(
            "opt cycles: "
            f"{min(opt_cycles)}..{max(opt_cycles)} avg={sum(opt_cycles) // len(opt_cycles)}"
        )
        print(f"aggregate speedup: {sum(ref_cycles) / sum(opt_cycles):.2f}x")

    labeled = [result for result in results if result.get("label") is not None]
    if labeled:
        label_matches = sum(1 for result in labeled if result.get("label_match") is True)
        print(f"label matches: {label_matches}/{len(labeled)}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("port", help="serial port, for example /dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument("--inter-frame-delay", type=float, default=0.03)
    parser.add_argument(
        "--vectors",
        type=Path,
        default=DEFAULT_VECTOR_HEADER,
        help="path to mnist_fc_test_vectors.h for the fixed-vector mode",
    )
    parser.add_argument(
        "--vector",
        action="append",
        type=int,
        help="fixed-vector index to send; may be repeated; default sends all fixed vectors",
    )
    parser.add_argument(
        "--image",
        action="append",
        type=Path,
        help="send one binary PGM/P5 28x28 image; may be repeated",
    )
    parser.add_argument(
        "--images-dir",
        nargs="?",
        const=DEFAULT_IMAGE_DIR,
        type=Path,
        help="send all .pgm images in a directory; default is mnist_fc/test_images_pgm",
    )
    parser.add_argument(
        "--labels",
        type=Path,
        help="optional labels.csv with file,label columns for image mode",
    )
    parser.add_argument("--limit", type=int, help="limit the number of image samples sent")
    parser.add_argument(
        "--require-label-match",
        action="store_true",
        help="treat image-mode model misclassification as a runner failure",
    )
    parser.add_argument("--jsonl", action="store_true", help="print parsed results as JSON lines")
    args = parser.parse_args()

    image_mode = args.image is not None or args.images_dir is not None
    if image_mode:
        samples = load_image_samples(args.image, args.images_dir, args.labels, args.limit)
    else:
        samples = select_vectors(load_test_vectors(args.vectors), args.vector)

    with serial.Serial(args.port, args.baud, timeout=0.2) as port:
        time.sleep(0.2)
        port.reset_input_buffer()

        ping = send_frame(port, CMD_PING, b"", args.timeout)
        print(ping)
        if not ping.startswith("OK "):
            raise SystemExit(1)

        failures = 0
        results: list[dict[str, object]] = []
        for sample in samples:
            line = send_frame(port, CMD_INFER, sample.payload, args.timeout)
            ok, result = validate_infer_response(
                line,
                sample,
                require_label_match=image_mode and args.require_label_match,
            )
            results.append(result)
            if args.jsonl:
                print(json.dumps(json_ready(result), sort_keys=True))
            else:
                print(format_sample_result(ok, sample, result))
            if not ok:
                failures += 1
            time.sleep(args.inter_frame_delay)

    print_summary(results, failures)
    if failures != 0:
        raise SystemExit(f"{failures} MNIST UART inference sample(s) failed")


if __name__ == "__main__":
    main()
