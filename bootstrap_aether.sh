#!/usr/bin/env bash
# bootstrap_aether.sh — Local Aether toolchain installer
#
# Downloads, builds, and sets up Aether locally in .aether/.
# Use 'export AETHERC="$(pwd)/.aether/build/aetherc"' before running build.sh
# if you want to use the local toolchain instead of the system-wide one.

set -e

DEST_DIR="$(pwd)/.aether"
mkdir -p "$DEST_DIR"

# 1. Download and extract
echo "=== Bootstrapping Aether into $DEST_DIR ==="
curl -L https://github.com/aether-lang-org/aether/archive/refs/heads/main.tar.gz -o "$DEST_DIR/aether.tar.gz"
tar -xzf "$DEST_DIR/aether.tar.gz" -C "$DEST_DIR" --strip-components=1
rm "$DEST_DIR/aether.tar.gz"

# 2. Build
echo "Building Aether..."
cd "$DEST_DIR"
make -j$(nproc)
make contrib

echo ""
echo "Bootstrap complete."
echo "Compiler: $DEST_DIR/build/aetherc"
echo "Runtime: $DEST_DIR/runtime"
echo "Stdlib: $DEST_DIR/std"
echo ""
echo "To use this toolchain, set these before building:"
echo "export AETHERC=\"$DEST_DIR/build/aetherc\""
echo "export AETHER_INCLUDE_PATH=\"$DEST_DIR\""
echo "export AETHER_LIB_PATH=\"$DEST_DIR/build\""
