#!/usr/bin/env bash
# JIT check (AGENTS.md "Checks"): each cartridge runs on the ARM build under qemu-arm with the interpreter and with
# the JIT, and the two runs must agree exactly. CPU test cartridges (core/tests/cpu, built by run_tests.sh): the
# whole ISViewer output and the exit code. Games: the end state after FIELDS fields (registers, RDRAM, the cycle
# count: n64emu --hash).
#   bash core/tools/jit_check.sh                      the CPU test cartridges
#   FIELDS=120 bash core/tools/jit_check.sh GAME.z64 ...
set -euo pipefail
core="$(cd "$(dirname "$0")/.." && pwd)"
bash "$core/tools/arm_build.sh" > /dev/null
run() { qemu-arm -L /usr/arm-linux-gnueabihf -cpu cortex-a7 "$core/build-arm/n64emu" "$@"; }
fail=0
if [ "${1:-}" = plant ]; then  # the planted JIT bug (N64_PLANT=6) must make the check fail
    if N64_PLANT=6 bash "$0" > /dev/null 2>&1; then echo "PLANT 6 NOT CAUGHT"; exit 1; fi
    echo "plant 6 caught"; exit 0
fi
if [ $# = 0 ]; then
    set -- "$core"/build/tests/cpu/*.rom/*.z64
    for rom in "$@"; do
        t="$(basename "$rom" .z64)"
        a=$(run "$rom" --until-isv --fields 100000 2>/dev/null; echo "rc $?")
        b=$(run "$rom" --until-isv --fields 100000 --jit 2>/dev/null; echo "rc $?")
        if [ "$a" = "$b" ] && echo "$a" | grep -q "^PASS"; then echo "same $t ($(echo "$b" | grep -c '^OUT') groups)"
        else echo "DIFFERENT $t"; diff <(echo "$a") <(echo "$b") | head -10; fail=1; fi
    done
    exit $fail
fi
for rom in "$@"; do
    a=$(run "$rom" --fields "${FIELDS:-60}" --hash 2>/dev/null | grep '^state')
    b=$(run "$rom" --fields "${FIELDS:-60}" --hash --jit --stats 2>/tmp/jit_check_stats.txt | grep '^state')
    if [ "$a" = "$b" ]; then echo "same $(basename "$rom"): $b"; grep '^jit:' /tmp/jit_check_stats.txt || true
    else echo "DIFFERENT $(basename "$rom")"; echo "  interpreter $a"; echo "  jit         $b"; fail=1; fi
done
exit $fail
