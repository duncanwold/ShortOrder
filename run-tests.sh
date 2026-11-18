#!/bin/bash
# Run the offline test harness. Outputs (WAV + CSV) go to build/test-output/.
#   ./run-tests.sh                  all tests
#   ./run-tests.sh --only detector  one test
#   ./run-tests.sh --bench          CPU benchmark only
set -e
cd "$(dirname "$0")"

if [ ! -f build/CMakeCache.txt ]; then
    cmake -B build -DCMAKE_BUILD_TYPE=Release
fi
cmake --build build --config Release --target GTTTests -j 8 > /dev/null

BIN="build/GTTTests_artefacts/Release/GTTTests"
[ -x "$BIN" ] || BIN="build/GTTTests_artefacts/GTTTests"
"$BIN" build/test-output "$@"
