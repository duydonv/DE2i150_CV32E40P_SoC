CROSS ?= riscv64-unknown-elf-

CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy

# CV32E40P implements RV32IMC; use this exact march so the compiler can
# emit compressed instructions + M-extension (mul/div) freely.
ARCH_FLAGS := -march=rv32imc -mabi=ilp32
CFLAGS     := -Os -ffreestanding -nostdlib -Wall -Wextra $(ARCH_FLAGS)
LDFLAGS    := -Wl,--build-id=none,-Bstatic,-T,sections.lds,--strip-debug

.PHONY: all clean

all: firmware.hex firmware_byte0.hex firmware_byte1.hex firmware_byte2.hex firmware_byte3.hex

firmware.elf: start.s main.c sections.lds
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.s main.c

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
