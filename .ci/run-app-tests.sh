#!/bin/bash

# Configuration
TIMEOUT=5
TOOLCHAIN_TYPE=${TOOLCHAIN_TYPE:-gnu}

# Test a single app: build, run, check
# Returns: 0=passed, 1=failed, 2=build_failed
test_app() {
    local app=$1

    echo "=== Testing $app ($TOOLCHAIN_TYPE) ==="

    # Build phase
    echo "[+] Building..."
    make clean > /dev/null 2>&1
    if ! make "$app" TOOLCHAIN_TYPE="$TOOLCHAIN_TYPE" > /dev/null 2>&1; then
        echo "[!] Build failed"
        return 2
    fi

    # Run phase
    echo "[+] Running in QEMU (timeout: ${TIMEOUT}s)..."
    local output exit_code
    output=$(timeout ${TIMEOUT}s qemu-system-riscv32 -nographic -machine virt -bios none -kernel build/image.elf 2>&1)
    exit_code=$?

    # Check phase
    if echo "$output" | grep -qiE "(trap|exception|fault|panic|illegal|segfault)"; then
        echo "[!] Crash detected"
        return 1
    elif [ $exit_code -eq 124 ] || [ $exit_code -eq 0 ]; then
        echo "[✓] Passed"
        return 0
    else
        echo "[!] Exit code $exit_code"
        return 1
    fi
}

# Auto-discover apps if none provided
if [ $# -eq 0 ]; then
    APPS=$(find app/ -name "*.c" -exec basename {} .c \; | sort | tr '\n' ' ')
    echo "[+] Auto-discovered apps: $APPS"
else
    APPS="$@"
fi

# Filter excluded apps
EXCLUDED_APPS="umode"
if [ -n "$EXCLUDED_APPS" ]; then
    FILTERED_APPS=""
    for app in $APPS; do
        [[ ! " $EXCLUDED_APPS " =~ " $app " ]] && FILTERED_APPS="$FILTERED_APPS $app"
    done
    APPS="$FILTERED_APPS"
fi

echo "[+] Testing apps: $APPS"
echo "[+] Toolchain: $TOOLCHAIN_TYPE"
echo ""

# Track results
PASSED_APPS=""
FAILED_APPS=""
BUILD_FAILED_APPS=""

# Test each app
for app in $APPS; do
    test_app "$app"
    case $? in
        0) PASSED_APPS="$PASSED_APPS $app" ;;
        1) FAILED_APPS="$FAILED_APPS $app" ;;
        2) BUILD_FAILED_APPS="$BUILD_FAILED_APPS $app" ;;
    esac
    echo ""
done

# Summary
echo "=== STEP 2 APP TEST RESULTS ==="
[ -n "$PASSED_APPS" ] && echo "[✓] PASSED:$PASSED_APPS"
[ -n "$FAILED_APPS" ] && echo "[!] FAILED (crashes):$FAILED_APPS"
[ -n "$BUILD_FAILED_APPS" ] && echo "[!] BUILD FAILED:$BUILD_FAILED_APPS"

# Parseable output
echo ""
echo "=== PARSEABLE_OUTPUT ==="
for app in $APPS; do
    if echo "$PASSED_APPS" | grep -qw "$app"; then
        echo "APP_STATUS:$app=passed"
    elif echo "$FAILED_APPS" | grep -qw "$app"; then
        echo "APP_STATUS:$app=failed"
    elif echo "$BUILD_FAILED_APPS" | grep -qw "$app"; then
        echo "APP_STATUS:$app=build_failed"
    fi
done

# Exit status
if [ -n "$FAILED_APPS" ] || [ -n "$BUILD_FAILED_APPS" ]; then
    echo ""
    echo "[!] Step 2 validation FAILED"
    exit 1
else
    echo ""
    echo "[+] Step 2 validation PASSED"
fi
