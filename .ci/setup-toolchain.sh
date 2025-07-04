#!/bin/bash
set -e

# Default to GNU if no toolchain specified
TOOLCHAIN_TYPE=${1:-gnu}

setup_gnu_toolchain() {
    echo "[+] Setting up GNU RISC-V toolchain..."

    local URL="https://github.com/riscv-collab/riscv-gnu-toolchain/releases/download/2025.07.03/riscv32-elf-ubuntu-24.04-gcc-nightly-2025.06.13-nightly.tar.xz"

    echo "[+] Downloading RISC-V GNU toolchain..."
    wget -q "$URL"
    tar -xf "$(basename "$URL")"

    echo "[+] Exporting GNU toolchain path..."
    echo "$PWD/riscv/bin" >> "$GITHUB_PATH"

    # Set cross-compile prefix for GNU
    echo "CROSS_COMPILE=riscv32-unknown-elf-" >> "$GITHUB_ENV"
    echo "TOOLCHAIN_TYPE=gnu" >> "$GITHUB_ENV"
}

setup_llvm_toolchain() {
    echo "[+] Setting up LLVM RISC-V toolchain..."

    # upstream URL for LLVM toolchain
    local URL="https://github.com/riscv-collab/riscv-gnu-toolchain/releases/download/2025.07.03/riscv32-elf-ubuntu-24.04-llvm-nightly-2025.06.13-nightly.tar.xz"

    echo "[+] Downloading RISC-V LLVM toolchain..."
    wget -q "$URL"
    tar -xf "$(basename "$URL")"

    echo "[+] Exporting LLVM toolchain path..."
    echo "$PWD/riscv/bin" >> "$GITHUB_PATH"

    # Set cross-compile prefix for LLVM
    echo "CROSS_COMPILE=llvm-" >> "$GITHUB_ENV"
    echo "TOOLCHAIN_TYPE=llvm" >> "$GITHUB_ENV"
}

case "$TOOLCHAIN_TYPE" in
    "gnu")
        setup_gnu_toolchain
        ;;
    "llvm")
        setup_llvm_toolchain
        ;;
    *)
        echo "Error: Unknown toolchain type '$TOOLCHAIN_TYPE'"
        echo "Usage: $0 [gnu|llvm]"
        exit 1
        ;;
esac

echo "[+] Toolchain setup complete: $TOOLCHAIN_TYPE"
