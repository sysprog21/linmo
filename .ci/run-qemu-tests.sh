#!/bin/bash
set -e

# Auto-discover apps if none provided
if [ $# -eq 0 ]; then
    # Get all .c files from app/ directory and extract names
    APPS=$(find app/ -name "*.c" -exec basename {} .c \; | sort | tr '\n' ' ')
    echo "[+] Auto-discovered apps: $APPS"
else
    APPS="$@"
fi

# Exclusion list for problematic test cases (if any)
EXCLUDED_APPS=""  # Add any apps that should be skipped here

# Filter out excluded apps
if [ -n "$EXCLUDED_APPS" ]; then
    FILTERED_APPS=""
    for app in $APPS; do
        if [[ ! " $EXCLUDED_APPS " =~ " $app " ]]; then
            FILTERED_APPS="$FILTERED_APPS $app"
        fi
    done
    APPS="$FILTERED_APPS"
fi

TIMEOUT=5
TOOLCHAIN_TYPE=${TOOLCHAIN_TYPE:-gnu}

echo "[+] Will run apps: $APPS"
echo "[+] Using toolchain: $TOOLCHAIN_TYPE"
echo ""

# Track test results
PASSED_APPS=""
FAILED_APPS=""
BUILD_FAILED_APPS=""

# Loop through each app
for app in $APPS; do
    echo "=== Running $app ($TOOLCHAIN_TYPE) ==="

    # Build the app
    echo "[+] Building $app with $TOOLCHAIN_TYPE toolchain..."
    make clean >/dev/null 2>&1
    if ! make "$app" TOOLCHAIN_TYPE="$TOOLCHAIN_TYPE" >/dev/null 2>&1; then
        echo "[!] Failed to build $app with $TOOLCHAIN_TYPE"
        BUILD_FAILED_APPS="$BUILD_FAILED_APPS $app"
        echo ""
        continue
    fi

    # Run in QEMU with timeout and capture output
    echo "[+] Running $app in QEMU (timeout: ${TIMEOUT}s)..."
    set +e
    output=$(timeout ${TIMEOUT}s qemu-system-riscv32 -nographic -machine virt -bios none -kernel build/image.elf 2>&1)
    exit_code=$?
    set -e

    # Check for kernel trap messages in output (common exception indicators)
    if echo "$output" | grep -qi -E "(trap|exception|fault|panic|illegal|segfault)"; then
        echo "[!] $app triggered kernel exception/trap"
        FAILED_APPS="$FAILED_APPS $app"
    elif [ $exit_code -eq 124 ]; then
        echo "[✓] $app timed out (acceptable - no crash)"
        PASSED_APPS="$PASSED_APPS $app"
    elif [ $exit_code -eq 0 ]; then
        echo "[✓] $app completed successfully"
        PASSED_APPS="$PASSED_APPS $app"
    else
        echo "[!] $app failed with exit code $exit_code"
        FAILED_APPS="$FAILED_APPS $app"
    fi
    echo ""
done

# Final validation results
echo "=== STEP 2 VALIDATION RESULTS ==="
if [ -n "$PASSED_APPS" ]; then
    echo "[✓] PASSED: $PASSED_APPS"
fi
if [ -n "$FAILED_APPS" ]; then
    echo "[!] FAILED (exceptions/crashes): $FAILED_APPS"
fi
if [ -n "$BUILD_FAILED_APPS" ]; then
    echo "[!] BUILD FAILED: $BUILD_FAILED_APPS"
fi

# Exit with error if any apps failed or had exceptions
if [ -n "$FAILED_APPS" ] || [ -n "$BUILD_FAILED_APPS" ]; then
    echo ""
    echo "[!] Step 2 validation FAILED - some apps crashed or had exceptions"
    exit 1
else
    echo ""
    echo "[+] Step 2 validation PASSED - all apps finished cleanly or timed out"
fi
