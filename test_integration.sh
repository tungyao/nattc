#!/bin/bash
set -e
cd "$(dirname "$0")"

echo "=== Building ==="
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)

echo "=== Running unit tests ==="
./test_token_bucket
./test_zcpool

echo "=== Checking binary sizes ==="
ls -lh p2p_client p2p_server

echo "=== All tests passed ==="
