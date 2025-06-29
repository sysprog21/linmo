#!/bin/bash
set -e
export CROSS_COMPILE=${CROSS_COMPILE:-riscv32-unknown-elf-}

# Check if at least one app is provided
if [ $# -eq 0 ]; then
    echo "Usage: $0 <app1> [app2] [app3] ..."
    echo "Example: $0 hello echo cpubench"
    exit 1
fi

APPS="$@"
TIMEOUT=10

echo "[+] Will run apps: $APPS"
echo ""

# Loop through each app
for app in $APPS; do
    echo "=== Running $app ==="

    # Build the app
    echo "[+] Building $app..."
    make clean >/dev/null 2>&1
    if ! make "$app" >/dev/null 2>&1; then
        echo "[!] Failed to build $app"
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

echo "[+] All apps tested"
