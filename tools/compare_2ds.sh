#!/usr/bin/env bash
# compare_2ds.sh JOB: the OUT hashes a 2DS --tests job printed against qemu's (core/tests/cpu/run_tests.sh first).
set -euo pipefail
core="$(cd "$(dirname "$0")/../core" && pwd)"
a=$(mktemp); b=$(mktemp)
grep -h '^# isv OUT' "$core/out/relay_$1.csv" | sed 's/^# isv //' | sort > "$a"
grep -h '^OUT' "$core"/build/tests/cpu/*.lnx.txt | sort > "$b"
echo "2DS $(wc -l < "$a") groups, qemu $(wc -l < "$b") groups"
if diff "$a" "$b"; then echo "2DS OUT hashes identical to qemu's"; else echo "DIFFERENCES above"; fi
grep -h '# test .*: ' "$core/out/relay_$1.csv" || true
rm -f "$a" "$b"
