#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

OUTPUT_DIR="dist"
rm -rf "$OUTPUT_DIR"
mkdir -p "$OUTPUT_DIR"

echo "========================================="
echo " Building for all platforms"
echo "========================================="

# ---- Native x86_64 Linux ----
echo ""
echo "[1/3] Native x86_64 Linux..."
rm -rf build_native
cmake -B build_native -S . -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build_native --target p2p_client p2p_server -j"$(nproc)" 2>&1 | grep -v "^$"
cp build_native/p2p_client "$OUTPUT_DIR/p2p_client_linux_x86_64"
cp build_native/p2p_server "$OUTPUT_DIR/p2p_server_linux_x86_64"
echo "  -> $OUTPUT_DIR/p2p_client_linux_x86_64"

# ---- AArch64 Linux (static) ----
echo ""
echo "[2/3] AArch64 Linux (static)..."
rm -rf build_aarch64
cmake -B build_aarch64 -S . \
    -DCMAKE_TOOLCHAIN_FILE=./aarch64-toolchain.cmake \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build_aarch64 --target p2p_client p2p_server -j"$(nproc)" 2>&1 | grep -v "^$"
cp build_aarch64/p2p_client "$OUTPUT_DIR/p2p_client_linux_aarch64"
cp build_aarch64/p2p_server "$OUTPUT_DIR/p2p_server_linux_aarch64"
echo "  -> $OUTPUT_DIR/p2p_client_linux_aarch64"

# ---- Windows x86_64 (static) ----
echo ""
echo "[3/3] Windows x86_64 (static)..."
rm -rf build_win64
cmake -B build_win64 -S . \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_EXE_LINKER_FLAGS="-static" \
    -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build_win64 --target p2p_client p2p_server -j"$(nproc)" 2>&1 | grep -v "^$"
cp build_win64/p2p_client.exe "$OUTPUT_DIR/p2p_client_windows_x86_64.exe"
cp build_win64/p2p_server.exe "$OUTPUT_DIR/p2p_server_windows_x86_64.exe"
echo "  -> $OUTPUT_DIR/p2p_client_windows_x86_64.exe"

echo ""
echo "========================================="
echo " All builds complete! Files in $OUTPUT_DIR/"
echo "========================================="
ls -lh "$OUTPUT_DIR/"
