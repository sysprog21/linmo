#!/bin/bash
set -e

# Default to GNU if no toolchain specified
TOOLCHAIN_TYPE=${1:-gnu}

TOOLCHAIN_REPO=https://github.com/riscv-collab/riscv-gnu-toolchain
TOOLCHAIN_VERSION=2025.05.30
TOOLCHAIN_OS=ubuntu-24.04

setup_gnu_toolchain() {
    echo "[+] Setting up GNU RISC-V toolchain..."

    local URL="${TOOLCHAIN_REPO}/releases/download/${TOOLCHAIN_VERSION}/riscv32-elf-${TOOLCHAIN_OS}-gcc-nightly-${TOOLCHAIN_VERSION}-nightly.tar.xz"

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

    # upstream URL for LLVM toolchainzz2
    local URL="${TOOLCHAIN_REPO}/releases/download/${TOOLCHAIN_VERSION}/riscv32-elf-${TOOLCHAIN_OS}-llvm-nightly-${TOOLCHAIN_VERSION}-nightly.tar.xz"

    echo "[+] Downloading RISC-V LLVM toolchain..."
    wget -q "$URL"
    tar -xf "$(basename "$URL")"

    echo "[+] Exporting LLVM toolchain path..."
    echo "$PWD/riscv/bin" >> "$GITHUB_PATH"

    # Set cross-compile prefix for LLVM
    echo "CROSS_COMPILE=riscv32-unknown-elf-" >> "$GITHUB_ENV"
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
