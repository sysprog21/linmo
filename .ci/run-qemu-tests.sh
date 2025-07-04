#!/bin/bash
set -e

# Check if at least one app is provided
if [ $# -eq 0 ]; then
    echo "Usage: $0 <app1> [app2] [app3] ..."
    echo "Example: $0 hello echo cpubench"
    exit 1
fi

APPS="$@"
TIMEOUT=10
TOOLCHAIN_TYPE=${TOOLCHAIN_TYPE:-gnu}

echo "[+] Will run apps: $APPS"
echo "[+] Using toolchain: $TOOLCHAIN_TYPE"
echo ""

# Loop through each app
for app in $APPS; do
    echo "=== Running $app ($TOOLCHAIN_TYPE) ==="

    # Build the app
    echo "[+] Building $app with $TOOLCHAIN_TYPE toolchain..."
    make clean >/dev/null 2>&1
    if ! make "$app" TOOLCHAIN_TYPE="$TOOLCHAIN_TYPE" >/dev/null 2>&1; then
        echo "[!] Failed to build $app with $TOOLCHAIN_TYPE"
        echo ""
        continue
    fi

    # Run in QEMU with timeout
    echo "[+] Running $app in QEMU (timeout: ${TIMEOUT}s)..."
    set +e
    timeout ${TIMEOUT}s qemu-system-riscv32 -nographic -machine virt -bios none -kernel build/image.elf
    exit_code=$?
    set -e

    # Print result
    if [ $exit_code -eq 124 ]; then
        echo "[!] $app timed out"
    elif [ $exit_code -eq 0 ]; then
        echo "[✓] $app completed successfully"
    else
        echo "[!] $app failed with exit code $exit_code"
    fi
    echo ""
done

echo "[+] All apps tested with $TOOLCHAIN_TYPE toolchain"
