#!/bin/bash
set -e

URL="https://github.com/riscv-collab/riscv-gnu-toolchain/releases/download/2025.06.13/riscv32-elf-ubuntu-22.04-gcc-nightly-2025.06.13-nightly.tar.xz"

echo "[+] Downloading RISC-V GNU toolchain..."
wget "$URL"
tar -xf "$(basename "$URL")"

echo "[+] Exporting toolchain path..."
echo "$PWD/riscv/bin" >> "$GITHUB_PATH"
