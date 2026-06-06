CROSS ?= riscv64-unknown-elf-
HOST_CXX ?= g++

CC      := $(CROSS)gcc
CXX     := $(CROSS)g++
AR      := $(CROSS)ar
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

PROGRAM ?= smoke

# Keep the smoke firmware word-aligned. CV32E40P supports compressed
# instructions, but the hardware-loop controller compares aligned 32-bit PCs,
# so this bring-up test is built without C-extension instructions.
ARCH_FLAGS := -march=rv32im_zicsr -mabi=ilp32
COMMON_CFLAGS := -ffreestanding -nostdlib -Wall -Wextra $(ARCH_FLAGS)
LDFLAGS    := -Wl,--build-id=none,-Bstatic,-T,sections.lds,--strip-debug
TFLM_LDFLAGS := -nostartfiles \
	-Wl,--build-id=none,-Bstatic,-T,sections.lds,--strip-debug,--gc-sections

ifeq ($(PROGRAM),smoke)
MAIN_SRC := main.c
OPT_FLAGS := -Os
PROGRAM_KIND := c
else ifeq ($(PROGRAM),benchmark)
MAIN_SRC := benchmark.c
OPT_FLAGS := -O2
PROGRAM_KIND := c
else ifeq ($(PROGRAM),tiny_ai)
MAIN_SRC := tiny_ai.c
OPT_FLAGS := -O2
PROGRAM_KIND := c
else ifeq ($(PROGRAM),rx_smoke)
MAIN_SRC := rx_smoke.c
OPT_FLAGS := -O2
PROGRAM_KIND := c
else ifeq ($(PROGRAM),tflm_hello)
MAIN_SRC := tflm_hello.cc
OPT_FLAGS := -Os
PROGRAM_KIND := tflm
else ifeq ($(PROGRAM),tflm_tiny_ai)
MAIN_SRC := tflm_tiny_ai.cc
OPT_FLAGS := -Os
PROGRAM_KIND := tflm
else ifeq ($(PROGRAM),tflm_tiny_uart)
MAIN_SRC := tflm_tiny_uart.cc
OPT_FLAGS := -Os
PROGRAM_KIND := tflm
else
$(error Unknown PROGRAM '$(PROGRAM)'. Use PROGRAM=smoke, PROGRAM=benchmark, PROGRAM=tiny_ai, PROGRAM=rx_smoke, PROGRAM=tflm_hello, PROGRAM=tflm_tiny_ai, or PROGRAM=tflm_tiny_uart)
endif

EXTRA_CFLAGS ?=
CFLAGS := $(OPT_FLAGS) $(COMMON_CFLAGS) $(EXTRA_CFLAGS)

TFLM_TREE ?= ../third_party/tflm_tree
TFLM_SRC_LIST := tflm_sources_minimal.txt
TFLM_OBJ_DIR := build/tflm
TFLM_HOST_DIR := build/host
TFLM_INCLUDES := \
	-I$(TFLM_TREE) \
	-I$(TFLM_TREE)/third_party/flatbuffers/include \
	-I$(TFLM_TREE)/third_party/gemmlowp \
	-I$(TFLM_TREE)/third_party/ruy \
	-I$(TFLM_TREE)/third_party/kissfft \
	-I$(TFLM_TREE)/examples/hello_world
TFLM_CXXFLAGS := $(OPT_FLAGS) $(ARCH_FLAGS) -std=c++17 -Wall -Wextra \
	-Wno-unused-parameter \
	-fno-exceptions -fno-rtti -fno-threadsafe-statics \
	-fno-use-cxa-atexit -ffunction-sections -fdata-sections \
	-DTF_LITE_STRIP_ERROR_STRINGS=1 -DTF_LITE_STATIC_MEMORY=1 \
	-DNDEBUG $(EXTRA_CFLAGS) $(TFLM_INCLUDES)

ifneq (,$(filter $(PROGRAM),tflm_tiny_ai tflm_tiny_uart))
TFLM_MODEL_SRC := tflm_tiny_ai_model_data.cc
TFLM_MODEL_HDR := tflm_tiny_ai_model_data.h
TFLM_MODEL_OBJ := $(TFLM_OBJ_DIR)/tflm_tiny_ai_model_data.o
TFLM_EXTRA_DEPS := tiny_ai_model.h ai_ops.h
else
TFLM_MODEL_SRC := $(TFLM_TREE)/examples/hello_world/models/hello_world_int8_model_data.cc
TFLM_MODEL_HDR := $(TFLM_TREE)/examples/hello_world/models/hello_world_int8_model_data.h
TFLM_MODEL_OBJ := $(TFLM_OBJ_DIR)/hello_world_int8_model_data.o
TFLM_EXTRA_DEPS :=
endif

TFLM_MAIN_OBJ := $(TFLM_OBJ_DIR)/$(MAIN_SRC:.cc=.o)
TFLM_TINY_MODEL_GEN := $(TFLM_HOST_DIR)/generate_tflm_tiny_ai_model

.PHONY: all clean smoke benchmark tiny_ai rx_smoke tflm_hello tflm_tiny_ai tflm_tiny_uart tflm_tiny_model FORCE

all: firmware.hex firmware_byte0.hex firmware_byte1.hex firmware_byte2.hex firmware_byte3.hex

smoke:
	$(MAKE) PROGRAM=smoke clean all

benchmark:
	$(MAKE) PROGRAM=benchmark clean all

tiny_ai:
	$(MAKE) PROGRAM=tiny_ai clean all

rx_smoke:
	$(MAKE) PROGRAM=rx_smoke clean all

tflm_hello:
	$(MAKE) PROGRAM=tflm_hello clean all

tflm_tiny_ai:
	$(MAKE) PROGRAM=tflm_tiny_ai clean all

tflm_tiny_uart:
	$(MAKE) PROGRAM=tflm_tiny_uart clean all

tflm_tiny_model: tflm_tiny_ai_model_data.cc tflm_tiny_ai_model_data.h

ifeq ($(PROGRAM_KIND),tflm)

$(TFLM_OBJ_DIR):
	mkdir -p $@

$(TFLM_HOST_DIR):
	mkdir -p $@

$(TFLM_TINY_MODEL_GEN): generate_tflm_tiny_ai_model.cc tiny_ai_model.h Makefile | $(TFLM_HOST_DIR)
	$(HOST_CXX) -std=c++17 -O2 -Wall -Wextra \
		-I$(TFLM_TREE) \
		-I$(TFLM_TREE)/third_party/flatbuffers/include \
		$< -o $@

tflm_tiny_ai_model_data.cc tflm_tiny_ai_model_data.h &: $(TFLM_TINY_MODEL_GEN) tiny_ai_model.h
	$(TFLM_TINY_MODEL_GEN) .

$(TFLM_MAIN_OBJ): $(MAIN_SRC) perf.h $(TFLM_MODEL_HDR) $(TFLM_EXTRA_DEPS) Makefile | $(TFLM_OBJ_DIR)
	$(CXX) $(TFLM_CXXFLAGS) -c $< -o $@

$(TFLM_OBJ_DIR)/tflm_port.o: tflm_port.cc perf.h Makefile | $(TFLM_OBJ_DIR)
	$(CXX) $(TFLM_CXXFLAGS) -c $< -o $@

$(TFLM_OBJ_DIR)/tflm_kernel_util_shim.o: tflm_kernel_util_shim.cc Makefile | $(TFLM_OBJ_DIR)
	$(CXX) $(TFLM_CXXFLAGS) -c $< -o $@

$(TFLM_MODEL_OBJ): $(TFLM_MODEL_SRC) $(TFLM_MODEL_HDR) Makefile | $(TFLM_OBJ_DIR)
	$(CXX) $(TFLM_CXXFLAGS) -c $< -o $@

$(TFLM_OBJ_DIR)/libtflm.a: $(TFLM_SRC_LIST) Makefile | $(TFLM_OBJ_DIR)
	rm -f $@
	while IFS= read -r src; do \
		obj="$(TFLM_OBJ_DIR)/$$(printf '%s' "$$src" | sed 's#[/.]#_#g').o"; \
		$(CXX) $(TFLM_CXXFLAGS) -c "$(TFLM_TREE)/$$src" -o "$$obj" || exit 1; \
		$(AR) rcs $@ "$$obj" || exit 1; \
	done < $<

firmware.elf: FORCE start.s $(MAIN_SRC) tflm_port.cc tflm_kernel_util_shim.cc \
		$(TFLM_MODEL_SRC) $(TFLM_MODEL_HDR) \
		$(TFLM_MAIN_OBJ) \
		$(TFLM_OBJ_DIR)/tflm_port.o \
		$(TFLM_OBJ_DIR)/tflm_kernel_util_shim.o \
		$(TFLM_MODEL_OBJ) \
		$(TFLM_OBJ_DIR)/libtflm.a \
		sections.lds Makefile
	$(CXX) $(ARCH_FLAGS) $(TFLM_LDFLAGS) -o $@ start.s \
		$(TFLM_MAIN_OBJ) \
		$(TFLM_OBJ_DIR)/tflm_port.o \
		$(TFLM_OBJ_DIR)/tflm_kernel_util_shim.o \
		$(TFLM_MODEL_OBJ) \
		$(TFLM_OBJ_DIR)/libtflm.a -lm
	$(SIZE) $@

else

firmware.elf: FORCE start.s $(MAIN_SRC) ai_ops.h perf.h tiny_ai_model.h sections.lds Makefile
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ start.s $(MAIN_SRC)

endif

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
	rm -rf build

FORCE:
