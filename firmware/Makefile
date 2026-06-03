CROSS ?= riscv64-unknown-elf-

CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

PROGRAM ?= smoke

# Keep the smoke firmware word-aligned. CV32E40P supports compressed
# instructions, but the hardware-loop controller compares aligned 32-bit PCs,
# so this bring-up test is built without C-extension instructions.
ARCH_FLAGS := -march=rv32im_zicsr -mabi=ilp32
COMMON_CFLAGS := -ffreestanding -nostdlib -Wall -Wextra $(ARCH_FLAGS)
LDFLAGS    := -Wl,--build-id=none,-Bstatic,-T,sections.lds,--strip-debug

ifeq ($(PROGRAM),smoke)
MAIN_SRC := main.c
OPT_FLAGS := -Os
else ifeq ($(PROGRAM),benchmark)
MAIN_SRC := benchmark.c
OPT_FLAGS := -O2
else ifeq ($(PROGRAM),tiny_ai)
MAIN_SRC := tiny_ai.c
OPT_FLAGS := -O2
else
$(error Unknown PROGRAM '$(PROGRAM)'. Use PROGRAM=smoke, PROGRAM=benchmark, or PROGRAM=tiny_ai)
endif

EXTRA_CFLAGS ?=
CFLAGS := $(OPT_FLAGS) $(COMMON_CFLAGS) $(EXTRA_CFLAGS)

.PHONY: all clean smoke benchmark tiny_ai FORCE

all: firmware.hex firmware_byte0.hex firmware_byte1.hex firmware_byte2.hex firmware_byte3.hex

smoke:
	$(MAKE) PROGRAM=smoke clean all

benchmark:
	$(MAKE) PROGRAM=benchmark clean all

tiny_ai:
	$(MAKE) PROGRAM=tiny_ai clean all

firmware.elf: FORCE start.s $(MAIN_SRC) ai_ops.h perf.h tiny_ai_model.h sections.lds Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.s $(MAIN_SRC)

firmware.bin: firmware.elf
	$(OBJCOPY) -O binary $< $@

# Word-granular hex (useful for Questa / $readmemh of a 32-bit ROM)
firmware.hex: firmware.bin
	python3 -c "from pathlib import Path; d=Path('firmware.bin').read_bytes(); d+=b'\\x00'*((4-len(d)%4)%4); Path('firmware.hex').write_text(''.join(f'{int.from_bytes(d[i:i+4],\"little\"):08x}\\n' for i in range(0,len(d),4)))"

# Byte-lane hex for the 4 M9K banks in de2i150_cv32e40p_top.v
firmware_byte0.hex: firmware.bin
	python3 split_hex.py firmware.bin
firmware_byte1.hex: firmware_byte0.hex
firmware_byte2.hex: firmware_byte0.hex
firmware_byte3.hex: firmware_byte0.hex

clean:
	rm -f firmware.elf firmware.bin firmware.hex firmware_byte*.hex

FORCE:
