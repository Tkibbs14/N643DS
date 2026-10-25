#!/usr/bin/env bash
# Builds the PC frontend for 32-bit ARM Linux (ARMv6K + VFP, like the 3DS CPU) and runs it under qemu-arm, so the JIT
# can be run and checked on the PC (as gc3ds's core/tools/arm_build.sh).
#   bash core/tools/arm_build.sh                     build core/build-arm/n64emu
#   bash core/tools/arm_build.sh run ROM [options]   build, then run n64emu under qemu-arm
set -euo pipefail
cd "$(dirname "$0")/.."
make -s -j"$(nproc)" CC=arm-linux-gnueabihf-gcc \
    CFLAGS="-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -march=armv6k -mfpu=vfp -mfloat-abi=hard -marm -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE" \
    BUILD=build-arm build-arm/n64emu
if [ "${1:-}" = run ]; then
    shift
    exec qemu-arm -L /usr/arm-linux-gnueabihf -cpu cortex-a7 build-arm/n64emu "$@"
fi
