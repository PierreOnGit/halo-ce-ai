#!/bin/sh
# Builds tests/client_harness: OpenCE's chasm client, run outside the game.
# 32-bit, as the game is; needs clang, the 32-bit C library and a 32-bit SDL3.
#   tests/client_harness/build.sh [output]
set -e
here=$(cd "$(dirname "$0")" && pwd)
output=${1:-$here/harness}
clang -m32 -std=gnu99 -g -O1 -Wall -Wextra -Wno-unused-parameter -I"$here" -o "$output" \
	"$here/harness.c" "$here/opus_standin.c" "$here/../../opence/port/linux/src/chasm_client.c" -lSDL3 -lm -lpthread
echo "built $output"
