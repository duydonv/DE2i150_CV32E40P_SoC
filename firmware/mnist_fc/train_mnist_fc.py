#!/usr/bin/env python3
"""Train, quantize, and verify an INT8 MNIST FC model for TFLM firmware."""

from __future__ import annotations

import argparse
import json
import os
import random
from pathlib import Path
from typing import Iterable

import numpy as np


MODEL_NAME = "MNIST FC"
INPUT_DIM = 784
HIDDEN_DIM = 32
OUTPUT_DIM = 10
FNV_MIX_SEED = 0x4D4E4953


def mix32(state: int, value: int) -> int:
    state &= 0xFFFFFFFF
    value &= 0xFFFFFFFF
    state ^= (
        value + 0x9E3779B9 + ((state << 6) & 0xFFFFFFFF) + (state >> 2)
    ) & 0xFFFFFFFF
    state ^= state >> 16
    state = (state * 0x7FEB352D) & 0xFFFFFFFF
    state ^= state >> 15
    state = (state * 0x846CA68B) & 0xFFFFFFFF
    state ^= state >> 16
    return state & 0xFFFFFFFF


def sanitize_symbol(text: str) -> str:
    out = []
    for ch in text:
        if ch.isalnum():
            out.append(ch.lower())
        else:
            out.append("_")
    return "".join(out).strip("_")


def quantize_to_int8(values: np.ndarray, scale: float, zero_point: int) -> np.ndarray:
    if scale == 0:
        raise ValueError("quantization scale is zero")
    quantized = np.rint(values / scale + zero_point)
    quantized = np.clip(quantized, -128, 127)
    return quantized.astype(np.int8)


def output_bytes(output: np.ndarray) -> np.ndarray:
    return output.astype(np.int8).view(np.uint8)


def make_representative_dataset(
    x_train: np.ndarray, count: int
) -> Iterable[list[np.ndarray]]:
    for i in range(count):
        yield [x_train[i : i + 1].astype(np.float32)]


def write_c_array(
    source_path: Path,
    header_path: Path,
    data_path: Path,
    symbol: str,
) -> None:
    data = source_path.read_bytes()
    header_guard = f"{symbol.upper()}_H"

    header_path.write_text(
        f"#ifndef {header_guard}\n"
        f"#define {header_guard}\n\n"
        f"constexpr unsigned int g_{symbol}_data_size = {len(data)};\n"
        f"extern const unsigned char g_{symbol}_data[];\n\n"
        f"#endif\n"
    )

    lines = [f'#include "{header_path.name}"', "", f"alignas(16) const unsigned char g_{symbol}_data[] = {{"]
    for start in range(0, len(data), 12):
        chunk = data[start : start + 12]
        rendered = ", ".join(f"0x{value:02x}" for value in chunk)
        if start + 12 < len(data):
            rendered += ","
        lines.append(f"    {rendered}")
    lines.append("};")
    lines.append("")
    data_path.write_text("\n".join(lines))


def format_c_int8_array(values: np.ndarray, per_line: int = 16) -> str:
    flat = values.reshape(-1).astype(np.int16).tolist()
    lines = []
    for start in range(0, len(flat), per_line):
        chunk = flat[start : start + per_line]
        lines.append("        " + ", ".join(f"{value:4d}" for value in chunk))
    return ",\n".join(lines)


def format_c_u8_array(values: np.ndarray, per_line: int = 16) -> str:
    flat = values.reshape(-1).astype(np.uint8).tolist()
    lines = []
    for start in range(0, len(flat), per_line):
        chunk = flat[start : start + per_line]
        lines.append("        " + ", ".join(f"{value:4d}" for value in chunk))
    return ",\n".join(lines)


def write_test_vectors(
    path: Path,
    quantized_inputs: np.ndarray,
    labels: np.ndarray,
    preds: np.ndarray,
    outputs: np.ndarray,
    checksum: int,
) -> None:
    sample_count = quantized_inputs.shape[0]
    rendered_inputs = []
    for sample in quantized_inputs:
        rendered_inputs.append("    {\n" + format_c_int8_array(sample) + "\n    }")
    rendered_outputs = []
    for sample in outputs:
        rendered_outputs.append("    {\n" + format_c_u8_array(output_bytes(sample), 10) + "\n    }")

    path.write_text(
        "#ifndef MNIST_FC_TEST_VECTORS_H\n"
        "#define MNIST_FC_TEST_VECTORS_H\n\n"
        "#include <stdint.h>\n\n"
        f"#define MNIST_FC_INPUT_DIM {INPUT_DIM}u\n"
        f"#define MNIST_FC_HIDDEN_DIM {HIDDEN_DIM}u\n"
        f"#define MNIST_FC_OUTPUT_DIM {OUTPUT_DIM}u\n"
        f"#define MNIST_FC_TEST_VECTOR_COUNT {sample_count}u\n"
        f"#define MNIST_FC_TEST_CHECKSUM 0x{checksum:08x}u\n\n"
        "static const int8_t mnist_fc_test_inputs[MNIST_FC_TEST_VECTOR_COUNT][MNIST_FC_INPUT_DIM]\n"
        "    __attribute__((aligned(4))) = {\n"
        + ",\n".join(rendered_inputs)
        + "\n};\n\n"
        "static const uint8_t mnist_fc_test_labels[MNIST_FC_TEST_VECTOR_COUNT] = {\n"
        + format_c_u8_array(labels.astype(np.uint8), 16)
        + "\n};\n\n"
        "static const uint8_t mnist_fc_test_expected_classes[MNIST_FC_TEST_VECTOR_COUNT] = {\n"
        + format_c_u8_array(preds.astype(np.uint8), 16)
        + "\n};\n\n"
        "static const uint8_t mnist_fc_test_expected_output_bytes[MNIST_FC_TEST_VECTOR_COUNT][MNIST_FC_OUTPUT_DIM] = {\n"
        + ",\n".join(rendered_outputs)
        + "\n};\n\n"
        "#endif\n"
    )


def verify_tflite(tf, tflite_model: bytes, x_test: np.ndarray, y_test: np.ndarray, limit: int | None):
    interpreter = tf.lite.Interpreter(model_content=tflite_model)
    interpreter.allocate_tensors()
    input_detail = interpreter.get_input_details()[0]
    output_detail = interpreter.get_output_details()[0]

    input_scale, input_zero_point = input_detail["quantization"]
    output_scale, output_zero_point = output_detail["quantization"]
    if input_detail["dtype"] != np.int8 or output_detail["dtype"] != np.int8:
        raise ValueError(
            f"expected int8 input/output, got {input_detail['dtype']} and {output_detail['dtype']}"
        )

    count = len(x_test) if limit is None else min(limit, len(x_test))
    preds = np.zeros(count, dtype=np.uint8)
    outputs = np.zeros((count, OUTPUT_DIM), dtype=np.int8)
    checksum = FNV_MIX_SEED
    correct = 0

    for i in range(count):
        q_input = quantize_to_int8(x_test[i : i + 1], input_scale, input_zero_point)
        interpreter.set_tensor(input_detail["index"], q_input)
        interpreter.invoke()
        out = interpreter.get_tensor(output_detail["index"])[0].astype(np.int8)
        pred = int(np.argmax(out))

        preds[i] = pred
        outputs[i] = out
        correct += int(pred == int(y_test[i]))

        checksum = mix32(checksum, pred)
        checksum = mix32(checksum, int(y_test[i]))
        for j, byte in enumerate(output_bytes(out)):
            checksum = mix32(checksum, int(byte) ^ ((j * 0x045D9F3B) & 0xFFFFFFFF))

    return {
        "accuracy": correct / count,
        "correct": correct,
        "count": count,
        "checksum": checksum,
        "input_detail": input_detail,
        "output_detail": output_detail,
        "preds": preds,
        "outputs": outputs,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", default="/tmp/mnist_fc_artifacts")
    parser.add_argument("--epochs", type=int, default=10)
    parser.add_argument("--batch-size", type=int, default=128)
    parser.add_argument("--representative-count", type=int, default=500)
    parser.add_argument("--verify-limit", type=int, default=10000)
    parser.add_argument("--vector-count", type=int, default=32)
    parser.add_argument("--seed", type=int, default=1234)
    args = parser.parse_args()

    os.environ.setdefault("TF_CPP_MIN_LOG_LEVEL", "2")
    random.seed(args.seed)
    np.random.seed(args.seed)

    import tensorflow as tf

    tf.random.set_seed(args.seed)

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    (x_train, y_train), (x_test, y_test) = tf.keras.datasets.mnist.load_data()
    x_train = (x_train.reshape((-1, INPUT_DIM)).astype(np.float32) / 255.0)
    x_test = (x_test.reshape((-1, INPUT_DIM)).astype(np.float32) / 255.0)
    y_train = y_train.astype(np.uint8)
    y_test = y_test.astype(np.uint8)

    model = tf.keras.Sequential(
        [
            tf.keras.layers.Input(shape=(INPUT_DIM,), name="input"),
            tf.keras.layers.Dense(HIDDEN_DIM, activation="relu", name="fc1"),
            tf.keras.layers.Dense(OUTPUT_DIM, name="fc2"),
        ],
        name="mnist_fc_784_32_10",
    )
    model.compile(
        optimizer=tf.keras.optimizers.Adam(learning_rate=0.001),
        loss=tf.keras.losses.SparseCategoricalCrossentropy(from_logits=True),
        metrics=["accuracy"],
    )

    history = model.fit(
        x_train,
        y_train,
        epochs=args.epochs,
        batch_size=args.batch_size,
        validation_split=0.1,
        verbose=2,
    )

    float_loss, float_acc = model.evaluate(x_test, y_test, verbose=0)
    keras_path = out_dir / "mnist_fc_float.keras"
    model.save(keras_path)

    converter = tf.lite.TFLiteConverter.from_keras_model(model)
    converter.optimizations = [tf.lite.Optimize.DEFAULT]
    converter.representative_dataset = lambda: make_representative_dataset(
        x_train, args.representative_count
    )
    converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    converter.inference_input_type = tf.int8
    converter.inference_output_type = tf.int8
    tflite_model = converter.convert()

    tflite_path = out_dir / "mnist_fc_int8.tflite"
    tflite_path.write_bytes(tflite_model)

    verify = verify_tflite(tf, tflite_model, x_test, y_test, args.verify_limit)
    input_detail = verify["input_detail"]
    output_detail = verify["output_detail"]
    input_scale, input_zero_point = input_detail["quantization"]
    output_scale, output_zero_point = output_detail["quantization"]

    vector_count = min(args.vector_count, verify["count"])
    vector_inputs = quantize_to_int8(x_test[:vector_count], input_scale, input_zero_point)
    vector_preds = verify["preds"][:vector_count]
    vector_outputs = verify["outputs"][:vector_count]
    vector_checksum = FNV_MIX_SEED
    for i in range(vector_count):
        vector_checksum = mix32(vector_checksum, int(vector_preds[i]))
        vector_checksum = mix32(vector_checksum, int(y_test[i]))
        for j, byte in enumerate(output_bytes(vector_outputs[i])):
            vector_checksum = mix32(
                vector_checksum,
                int(byte) ^ ((j * 0x045D9F3B) & 0xFFFFFFFF),
            )

    write_c_array(
        tflite_path,
        out_dir / "mnist_fc_model_data.h",
        out_dir / "mnist_fc_model_data.cc",
        "mnist_fc_model",
    )
    write_test_vectors(
        out_dir / "mnist_fc_test_vectors.h",
        vector_inputs,
        y_test[:vector_count],
        vector_preds,
        vector_outputs,
        vector_checksum,
    )

    metadata = {
        "model_name": MODEL_NAME,
        "shape": {
            "input_dim": INPUT_DIM,
            "hidden_dim": HIDDEN_DIM,
            "output_dim": OUTPUT_DIM,
        },
        "seed": args.seed,
        "epochs": args.epochs,
        "batch_size": args.batch_size,
        "representative_count": args.representative_count,
        "float": {
            "loss": float(float_loss),
            "accuracy": float(float_acc),
        },
        "int8": {
            "accuracy": float(verify["accuracy"]),
            "correct": int(verify["correct"]),
            "count": int(verify["count"]),
            "checksum": f"0x{verify['checksum']:08x}",
            "model_bytes": len(tflite_model),
        },
        "test_vectors": {
            "count": int(vector_count),
            "checksum": f"0x{vector_checksum:08x}",
        },
        "input_quantization": {
            "scale": float(input_scale),
            "zero_point": int(input_zero_point),
            "dtype": str(input_detail["dtype"]),
            "shape": input_detail["shape"].astype(int).tolist(),
        },
        "output_quantization": {
            "scale": float(output_scale),
            "zero_point": int(output_zero_point),
            "dtype": str(output_detail["dtype"]),
            "shape": output_detail["shape"].astype(int).tolist(),
        },
        "artifacts": {
            "keras_model": keras_path.name,
            "tflite_model": tflite_path.name,
            "c_header": "mnist_fc_model_data.h",
            "c_source": "mnist_fc_model_data.cc",
            "test_vectors": "mnist_fc_test_vectors.h",
        },
        "history": history.history,
    }
    (out_dir / "mnist_fc_metadata.json").write_text(
        json.dumps(metadata, indent=2, sort_keys=True) + "\n"
    )

    print("MNIST FC host preparation complete")
    print(f"out_dir: {out_dir}")
    print(f"float_accuracy: {float_acc:.4f}")
    print(
        f"int8_accuracy: {verify['accuracy']:.4f} "
        f"({verify['correct']}/{verify['count']})"
    )
    print(f"int8_checksum: 0x{verify['checksum']:08x}")
    print(f"test_vector_checksum: 0x{vector_checksum:08x}")
    print(f"input_quant: scale={input_scale} zero_point={input_zero_point}")
    print(f"output_quant: scale={output_scale} zero_point={output_zero_point}")
    print(f"model_bytes: {len(tflite_model)}")


if __name__ == "__main__":
    main()
