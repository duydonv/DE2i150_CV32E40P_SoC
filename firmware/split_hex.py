"""Split a raw firmware.bin into 4 byte-lane hex files for the DE2i-150 BRAM.

The top-level SoC splits the 32-bit word at address WA into four byte lanes:
    mem0[WA] = byte 0 (bits  7: 0)   <- firmware_byte0.hex
    mem1[WA] = byte 1 (bits 15: 8)   <- firmware_byte1.hex
    mem2[WA] = byte 2 (bits 23:16)   <- firmware_byte2.hex
    mem3[WA] = byte 3 (bits 31:24)   <- firmware_byte3.hex

Each hex file has one byte per line, WORDS lines total. WORDS must match
MEM_WORDS in the SoC top (default 8192 = 32 KB / 4). Unused words are
zero-padded so $readmemh does not leave X in the M9K blocks.
"""
import sys
from pathlib import Path

BIN_PATH = Path(sys.argv[1] if len(sys.argv) > 1 else "firmware.bin")
WORDS    = 8192  # keep in sync with BRAM_AW_WORDS in the top module

data = BIN_PATH.read_bytes()
pad  = (4 - len(data) % 4) % 4
data = data + b"\x00" * pad

word_count = len(data) // 4
if word_count > WORDS:
    raise SystemExit(f"firmware.bin uses {word_count} words, BRAM has {WORDS}")

lanes = ([], [], [], [])
for i in range(WORDS):
    if i < word_count:
        lanes[0].append(data[4*i + 0])
        lanes[1].append(data[4*i + 1])
        lanes[2].append(data[4*i + 2])
        lanes[3].append(data[4*i + 3])
    else:
        for lane in lanes:
            lane.append(0)

for lane_idx, bytes_ in enumerate(lanes):
    out = Path(f"firmware_byte{lane_idx}.hex")
    out.write_text("\n".join(f"{b:02x}" for b in bytes_) + "\n")
    print(f"wrote {out} ({len(bytes_)} bytes)")
