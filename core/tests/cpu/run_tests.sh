#!/usr/bin/env bash
# CPU tests (docs/TEST_ROMS.md). Each test source is built twice: as a cartridge run on u64emu, and as a Linux n32
# program run on qemu-mipsn32 -cpu R4000. Their OUT lines must match; EXPECT failures (cartridge only) fail the test.
#   bash core/tests/cpu/run_tests.sh            all tests
#   bash core/tests/cpu/run_tests.sh alu fpu    some
#   VERBOSE=1 ...                               every value, to find the first mismatch (diff the two .txt files)
#   bash core/tests/cpu/run_tests.sh plant      each planted interpreter bug (U64_PLANT, vr4300.h) must fail its test
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
if [ "${1:-}" = plant ]; then
    fail=0
    for pair in 1:alu 2:mem 3:fpu 4:branch 5:alu; do
        n=${pair%%:*}; t=${pair#*:}
        if U64_PLANT=$n bash "$0" "$t" > /dev/null 2>&1; then echo "PLANT $n NOT CAUGHT by $t"; fail=1
        else echo "plant $n caught by $t"; fi
    done
    exit $fail
fi
root="$here/../.."
out="$root/build/tests/cpu"
mkdir -p "$out"
make -C "$root" -s
CC=mipsel-linux-gnu-gcc
LD=mipsel-linux-gnu-ld
OBJCOPY=mipsel-linux-gnu-objcopy
CFLAGS="-EB -march=vr4300 -mabi=n32 -O2 -ffreestanding -nostdlib -fno-pic -mno-abicalls -G0 -fno-builtin
        -mno-check-zero-division -fno-tree-loop-distribute-patterns -Wall -Wno-unused-function"
[ "${VERBOSE:-0}" = 1 ] && CFLAGS="$CFLAGS -DVERBOSE"
ROMONLY="exc cop0"   # tests that take exceptions: cartridge only, EXPECT lines only
tests="${*:-alu mem branch fpu exc cop0}"
fail=0
for t in $tests; do
    [ -f "$here/$t.c" ] || { echo "no test $t"; fail=1; continue; }
    for plat in rom lnx; do
        d="$out/$t.$plat"
        mkdir -p "$d"
        def=""; [ $plat = rom ] && def="-DPLAT_ROM"
        $CC $CFLAGS $def -c "$here/crt.S" -o "$d/crt.o"
        for s in crt_main plat $t; do $CC $CFLAGS $def -c "$here/$s.c" -o "$d/$s.o"; done
        objs="$d/crt.o $d/crt_main.o $d/plat.o $d/$t.o"
        if [[ " $ROMONLY " == *" $t "* ]]; then
            [ $plat = lnx ] && continue
            $CC $CFLAGS $def -c "$here/excvec.S" -o "$d/excvec.o"
            $CC $CFLAGS $def -c "$here/exc_install.c" -o "$d/exc_install.o"
            objs="$objs $d/excvec.o $d/exc_install.o"
        fi
        if [ $plat = rom ]; then
            $LD -EB -m elf32btsmipn32 --no-warn-rwx-segments -T "$here/rom.ld" $objs -o "$d/$t.elf"
            $OBJCOPY -O binary "$d/$t.elf" "$d/$t.bin"
            python3 "$here/mkrom.py" "$d/$t.bin" "$d/$t.z64"
        else
            $LD -EB -m elf32btsmipn32 --no-warn-rwx-segments -static -e _start $objs -o "$d/$t.elf"
        fi
    done
    rc=0
    "$root/build/u64emu" "$out/$t.rom/$t.z64" --until-isv --fields 100000 > "$out/$t.rom.txt" 2>"$out/$t.rom.err" || rc=$?
    if [[ " $ROMONLY " == *" $t "* ]]; then
        if [ $rc = 0 ]; then echo "PASS $t"; else echo "FAIL $t (rc $rc)"; grep -h FAIL "$out/$t.rom.txt" | head -20; fail=1; fi
        continue
    fi
    qemu-mipsn32 -cpu R4000 "$out/$t.lnx/$t.elf" > "$out/$t.lnx.txt" || { echo "FAIL $t: qemu run failed"; fail=1; continue; }
    if ! diff <(grep '^OUT' "$out/$t.rom.txt") <(grep '^OUT' "$out/$t.lnx.txt") > "$out/$t.diff"; then
        echo "FAIL $t: OUT lines differ from qemu ($out/$t.diff):"; head -20 "$out/$t.diff"; fail=1
    elif [ $rc != 0 ]; then
        echo "FAIL $t (rc $rc)"; grep -h FAIL "$out/$t.rom.txt" | head -20; tail -3 "$out/$t.rom.err"; fail=1
    else
        echo "PASS $t ($(grep -c '^OUT' "$out/$t.rom.txt") groups match qemu)"
    fi
done
exit $fail
