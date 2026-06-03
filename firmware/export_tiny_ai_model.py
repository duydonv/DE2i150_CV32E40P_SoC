#!/usr/bin/env python3
"""Export a tiny fixed INT8 model for the bare-metal inference demo."""

from __future__ import annotations

from pathlib import Path


INPUT_DIM = 64
HIDDEN_DIM = 16
OUTPUT_DIM = 4
SAMPLE_COUNT = 8

FC1_SHIFT = 9
FC2_SHIFT = 7
HIDDEN_CLAMP = 127
OUTPUT_CLAMP = 255


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


def make_sample(label: int, variant: int) -> list[int]:
    values = []
    for row in range(8):
        for col in range(8):
            value = 48 if is_quadrant(label, row, col) else -24
            if variant == 1:
                if is_quadrant(label, row, col) and (
                    (row * 3 + col * 5 + label) & 3
                ) == 0:
                    value += 8
                elif (row + col + label) % 7 == 0:
                    value -= 4
            values.append(clamp_i8(value))
    return values


def make_hidden_weight(hidden: int) -> list[int]:
    values = []
    for row in range(8):
        for col in range(8):
            if hidden < 4:
                value = 32 if is_quadrant(hidden, row, col) else -8
            elif hidden == 4:
                value = 16 if row < 4 else -8
            elif hidden == 5:
                value = 16 if row >= 4 else -8
            elif hidden == 6:
                value = 16 if col < 4 else -8
            elif hidden == 7:
                value = 16 if col >= 4 else -8
            else:
                cls = (hidden - 8) // 2
                inner = (row % 4) in (1, 2) and (col % 4) in (1, 2)
                edge = (row % 4) in (0, 3) or (col % 4) in (0, 3)
                if is_quadrant(cls, row, col) and ((hidden - 8) % 2 == 0) and inner:
                    value = 24
                elif is_quadrant(cls, row, col) and ((hidden - 8) % 2 == 1) and edge:
                    value = 20
                else:
                    value = -6
            values.append(value)
    return values


def make_fc2_weight() -> list[list[int]]:
    rows = []
    for out in range(OUTPUT_DIM):
        row = []
        for hidden in range(HIDDEN_DIM):
            if hidden == out:
                value = 56
            elif hidden in (4, 5, 6, 7):
                matches = (
                    (out in (0, 1) and hidden == 4)
                    or (out in (2, 3) and hidden == 5)
                    or (out in (0, 2) and hidden == 6)
                    or (out in (1, 3) and hidden == 7)
                )
                value = 12 if matches else -10
            elif hidden >= 8 and (hidden - 8) // 2 == out:
                value = 18
            else:
                value = -14
            row.append(value)
        rows.append(row)
    return rows


def clamp_relu(value: int, upper_bound: int) -> int:
    if value < 0:
        return 0
    return min(value, upper_bound)


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


def infer_one(
    sample: list[int],
    fc1_weight: list[list[int]],
    fc1_bias: list[int],
    fc2_weight: list[list[int]],
    fc2_bias: list[int],
) -> list[int]:
    hidden = []
    for weights, bias in zip(fc1_weight, fc1_bias):
        acc = bias + sum(a * w for a, w in zip(sample, weights))
        hidden.append(clamp_relu(acc >> FC1_SHIFT, HIDDEN_CLAMP))

    scores = []
    for weights, bias in zip(fc2_weight, fc2_bias):
        acc = bias + sum(a * w for a, w in zip(hidden, weights))
        scores.append(clamp_relu(acc >> FC2_SHIFT, OUTPUT_CLAMP))
    return scores


def format_1d(values: list[int], per_line: int = 16) -> str:
    lines = []
    for start in range(0, len(values), per_line):
        chunk = values[start : start + per_line]
        lines.append("        " + ", ".join(f"{value:4d}" for value in chunk))
    return ",\n".join(lines)


def format_2d(rows: list[list[int]], per_line: int = 16) -> str:
    rendered = []
    for row in rows:
        rendered.append("    {\n" + format_1d(row, per_line) + "\n    }")
    return ",\n".join(rendered)


def main() -> None:
    inputs = []
    labels = []
    for cls in range(OUTPUT_DIM):
        for variant in range(2):
            inputs.append(make_sample(cls, variant))
            labels.append(cls)

    fc1_weight = [make_hidden_weight(hidden) for hidden in range(HIDDEN_DIM)]
    fc1_bias = [0] * HIDDEN_DIM
    fc2_weight = make_fc2_weight()
    fc2_bias = [0, -64, -32, -96]

    checksum = 0x54494E59
    predictions = []
    sample_scores = []
    for sample in inputs:
        scores = infer_one(sample, fc1_weight, fc1_bias, fc2_weight, fc2_bias)
        pred = max(range(OUTPUT_DIM), key=lambda idx: scores[idx])
        predictions.append(pred)
        sample_scores.append(scores)
        checksum = mix32(checksum, pred)
        for idx, score in enumerate(scores):
            checksum = mix32(checksum, score ^ ((idx * 0x045D9F3B) & 0xFFFFFFFF))

    if predictions != labels:
        raise SystemExit(f"model does not classify export samples: {predictions}")

    header = f"""#ifndef TINY_AI_MODEL_H
#define TINY_AI_MODEL_H

#include <stdint.h>

#define TINY_AI_MODEL_NAME \"8x8 quadrant MLP\"
#define TINY_AI_INPUT_DIM {INPUT_DIM}u
#define TINY_AI_HIDDEN_DIM {HIDDEN_DIM}u
#define TINY_AI_OUTPUT_DIM {OUTPUT_DIM}u
#define TINY_AI_SAMPLE_COUNT {SAMPLE_COUNT}u
#define TINY_AI_FC1_SHIFT {FC1_SHIFT}u
#define TINY_AI_FC2_SHIFT {FC2_SHIFT}u
#define TINY_AI_HIDDEN_CLAMP {HIDDEN_CLAMP}u
#define TINY_AI_OUTPUT_CLAMP {OUTPUT_CLAMP}u
#define TINY_AI_EXPECTED_CHECKSUM 0x{checksum:08x}u

static const int8_t tiny_ai_expected_labels[TINY_AI_SAMPLE_COUNT] = {{
{format_1d(labels, 8)}
}};

static const int8_t tiny_ai_input_samples[TINY_AI_SAMPLE_COUNT][TINY_AI_INPUT_DIM]
    __attribute__((aligned(4))) = {{
{format_2d(inputs)}
}};

static const int32_t tiny_ai_fc1_bias[TINY_AI_HIDDEN_DIM]
    __attribute__((aligned(4))) = {{
{format_1d(fc1_bias, 8)}
}};

static const int8_t tiny_ai_fc1_weight[TINY_AI_HIDDEN_DIM][TINY_AI_INPUT_DIM]
    __attribute__((aligned(4))) = {{
{format_2d(fc1_weight)}
}};

static const int32_t tiny_ai_fc2_bias[TINY_AI_OUTPUT_DIM]
    __attribute__((aligned(4))) = {{
{format_1d(fc2_bias, 4)}
}};

static const int8_t tiny_ai_fc2_weight[TINY_AI_OUTPUT_DIM][TINY_AI_HIDDEN_DIM]
    __attribute__((aligned(4))) = {{
{format_2d(fc2_weight)}
}};

#endif
"""

    Path(__file__).with_name("tiny_ai_model.h").write_text(header)
    print(f"wrote tiny_ai_model.h")
    print(f"expected checksum: 0x{checksum:08x}")
    print("expected labels:", " ".join(str(label) for label in labels))
    print("sample0 scores:", " ".join(str(score) for score in sample_scores[0]))


if __name__ == "__main__":
    main()
