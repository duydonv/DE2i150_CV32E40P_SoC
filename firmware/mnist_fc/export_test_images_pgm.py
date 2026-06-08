#!/usr/bin/env python3
"""Export MNIST test split samples as binary PGM images."""

from __future__ import annotations

import argparse
import csv
import urllib.request
from pathlib import Path

import numpy as np


DEFAULT_URL = "https://storage.googleapis.com/tensorflow/tf-keras-datasets/mnist.npz"
DEFAULT_CACHE = Path.home() / ".cache" / "de2i150_cv32e40p_soc" / "mnist.npz"
DEFAULT_OUT = Path(__file__).resolve().parent / "test_images_pgm"


def ensure_dataset(url: str, cache: Path) -> Path:
    if cache.exists():
        return cache
    cache.parent.mkdir(parents=True, exist_ok=True)
    print(f"downloading {url} -> {cache}")
    urllib.request.urlretrieve(url, cache)
    return cache


def write_pgm(path: Path, pixels: np.ndarray) -> None:
    if pixels.shape != (28, 28):
        raise ValueError(f"expected 28x28 image, got {pixels.shape}")
    with path.open("wb") as f:
        f.write(b"P5\n28 28\n255\n")
        f.write(pixels.astype(np.uint8).tobytes())


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--start", type=int, default=32, help="first MNIST test index")
    parser.add_argument("--count", type=int, default=200, help="number of images to export")
    parser.add_argument("--out-dir", type=Path, default=DEFAULT_OUT)
    parser.add_argument("--cache", type=Path, default=DEFAULT_CACHE)
    parser.add_argument("--url", default=DEFAULT_URL)
    args = parser.parse_args()

    dataset = ensure_dataset(args.url, args.cache)
    with np.load(dataset) as data:
        x_test = data["x_test"]
        y_test = data["y_test"]

    end = args.start + args.count
    if args.start < 0 or end > len(x_test):
        raise ValueError(f"requested range {args.start}..{end - 1}, test set has {len(x_test)}")

    args.out_dir.mkdir(parents=True, exist_ok=True)
    with (args.out_dir / "labels.csv").open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["file", "mnist_test_index", "label"])
        for idx in range(args.start, end):
            label = int(y_test[idx])
            name = f"mnist_test_{idx:05d}_label{label}.pgm"
            write_pgm(args.out_dir / name, x_test[idx])
            writer.writerow([name, idx, label])

    print(f"wrote {args.count} PGM images and labels.csv to {args.out_dir}")


if __name__ == "__main__":
    main()
