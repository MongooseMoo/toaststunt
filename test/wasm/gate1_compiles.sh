#!/bin/bash
set -e
cd "$(dirname "$0")/../../build-wasm"
ls moo.js moo.wasm
test $(wc -c < moo.wasm) -gt 0
echo "GATE1_PASS"
