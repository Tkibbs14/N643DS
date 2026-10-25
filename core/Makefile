# PC build of the emulator core (run in WSL):  make -C core   ->  core/build/n64emu
CC      ?= gcc
CFLAGS  ?= -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS  += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDLIBS  := -lm
BUILD   ?= build
.DEFAULT_GOAL := $(BUILD)/n64emu

SRC := $(wildcard src/cpu/*.c) $(wildcard src/sys/*.c) $(wildcard src/jit/*.c) $(wildcard src/rsp/*.c) $(wildcard src/rdp/*.c) $(wildcard src/hle/*.c) $(wildcard src/host/*.c)
OBJ := $(patsubst src/%.c,$(BUILD)/obj/%.o,$(SRC))

$(BUILD)/n64emu: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# The FPU follows the guest's rounding mode through fesetround: no constant folding or fused multiply-adds across it.
$(BUILD)/obj/cpu/vr4300_fpu.o: override CFLAGS += -frounding-math -ffp-contract=off

$(BUILD)/obj/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf build

-include $(OBJ:.o=.d)
.PHONY: clean
