#!/usr/bin/env bash
# Builds n643ds.3dsx, with the relay's CA roots and the CPU test cartridges (core/tests/cpu, built by run_tests.sh)
# in RomFS.
#   bash core/host3ds/build.sh [make options]
set -euo pipefail
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
cd "$(dirname "$0")"
mkdir -p romfs/tests
cp certs/relay_ca.pem romfs/
tests=../build/tests/cpu
if ls $tests/*.rom/*.z64 > /dev/null 2>&1; then
    for t in $tests/*.rom/*.z64; do cp "$t" romfs/tests/; done
else
    echo "no CPU test cartridges (run core/tests/cpu/run_tests.sh first)"
fi
# RomFS contents are not tracked by make; force the final link so they are always packed.
rm -f n643ds.3dsx
make "$@"
