#!/bin/bash
set -e

# Functional test configuration
TIMEOUT=5
TOOLCHAIN_TYPE=${TOOLCHAIN_TYPE:-gnu}

# Define functional tests and their expected PASS criteria
declare -A FUNCTIONAL_TESTS
FUNCTIONAL_TESTS["mutex"]="Fairness: PASS,Mutual Exclusion: PASS,Data Consistency: PASS,Overall: PASS"

# Add more functional tests here as they are developed
# Format: FUNCTIONAL_TESTS["app_name"]="Expected Line 1: PASS,Expected Line 2: PASS,..."
#
# To add a new functional test:
# 1. Ensure your app outputs specific "Something: PASS" lines
# 2. Add the app and its expected PASS criteria to FUNCTIONAL_TESTS above
# 3. The script will automatically validate all required PASS lines appear
#
# Example entries for future functional tests:
# FUNCTIONAL_TESTS["semaphore"]="Basic Operations: PASS,FIFO Ordering: PASS,Overall: PASS"
# FUNCTIONAL_TESTS["pipes"]="Write/Read: PASS,Blocking: PASS,Overall: PASS"

echo "[+] Linmo Functional Test Suite (Step 3)"
echo "[+] Using toolchain: $TOOLCHAIN_TYPE"
echo "[+] Timeout: ${TIMEOUT}s per test"
echo ""

# Track test results
PASSED_TESTS=""
FAILED_TESTS=""
BUILD_FAILED_TESTS=""
TOTAL_TESTS=0

# Get list of functional tests to run
if [ $# -eq 0 ]; then
    # Run all defined functional tests
    TESTS_TO_RUN=$(echo "${!FUNCTIONAL_TESTS[@]}" | tr ' ' '\n' | sort | tr '\n' ' ')
    echo "[+] Running all functional tests: $TESTS_TO_RUN"
else
    # Run specified tests
    TESTS_TO_RUN="$@"
    echo "[+] Running specified tests: $TESTS_TO_RUN"
fi

echo ""

# Run each functional test
for test in $TESTS_TO_RUN; do
    TOTAL_TESTS=$((TOTAL_TESTS + 1))
    echo "=== Functional Test: $test ==="

    # Check if test is defined
    if [ -z "${FUNCTIONAL_TESTS[$test]}" ]; then
        echo "[!] Unknown functional test: $test (not in test definition)"
        FAILED_TESTS="$FAILED_TESTS $test"
        echo ""
        continue
    fi

    # Build the test app
    echo "[+] Building $test with $TOOLCHAIN_TYPE toolchain..."
    make clean >/dev/null 2>&1
    if ! make "$test" TOOLCHAIN_TYPE="$TOOLCHAIN_TYPE" >/dev/null 2>&1; then
        echo "[!] Failed to build $test with $TOOLCHAIN_TYPE"
        BUILD_FAILED_TESTS="$BUILD_FAILED_TESTS $test"
        echo ""
        continue
    fi

    # Run the test in QEMU and capture output
    echo "[+] Running $test functional test (timeout: ${TIMEOUT}s)..."
    set +e
    output=$(timeout ${TIMEOUT}s qemu-system-riscv32 -nographic -machine virt -bios none -kernel build/image.elf 2>&1)
    exit_code=$?
    set -e

    # Check for crashes/exceptions first
    if echo "$output" | grep -qi -E "(trap|exception|fault|panic|illegal|segfault)"; then
        echo "[!] $test crashed with exception/trap"
        FAILED_TESTS="$FAILED_TESTS $test"
        echo ""
        continue
    fi

    # Check if test ran (non-crash exit codes)
    if [ $exit_code -ne 124 ] && [ $exit_code -ne 0 ]; then
        echo "[!] $test failed with exit code $exit_code"
        FAILED_TESTS="$FAILED_TESTS $test"
        echo ""
        continue
    fi

    # Parse expected PASS criteria
    expected_passes="${FUNCTIONAL_TESTS[$test]}"
    IFS=',' read -ra PASS_CRITERIA <<< "$expected_passes"

    # Check each required PASS line
    all_passes_found=true
    missing_passes=""

    echo "[+] Checking for required PASS criteria:"
    for criteria in "${PASS_CRITERIA[@]}"; do
        if echo "$output" | grep -q "$criteria"; then
            echo "  ✓ Found: $criteria"
        else
            echo "  ✗ Missing: $criteria"
            all_passes_found=false
            missing_passes="$missing_passes '$criteria'"
        fi
    done

    # Determine test result
    if [ "$all_passes_found" = true ]; then
        echo "[✓] $test functional test PASSED"
        PASSED_TESTS="$PASSED_TESTS $test"
    else
        echo "[!] $test functional test FAILED - missing:$missing_passes"
        FAILED_TESTS="$FAILED_TESTS $test"
    fi

    echo ""
done

# Final results summary
echo "=== STEP 3 FUNCTIONAL TEST RESULTS ==="
echo "Total tests: $TOTAL_TESTS"

if [ -n "$PASSED_TESTS" ]; then
    passed_count=$(echo $PASSED_TESTS | wc -w)
    echo "[✓] PASSED ($passed_count):$PASSED_TESTS"
fi

if [ -n "$FAILED_TESTS" ]; then
    failed_count=$(echo $FAILED_TESTS | wc -w)
    echo "[!] FAILED ($failed_count):$FAILED_TESTS"
fi

if [ -n "$BUILD_FAILED_TESTS" ]; then
    build_failed_count=$(echo $BUILD_FAILED_TESTS | wc -w)
    echo "[!] BUILD FAILED ($build_failed_count):$BUILD_FAILED_TESTS"
fi

# Generate parseable output for summary
echo ""
echo "=== PARSEABLE_OUTPUT ==="
for test in $TESTS_TO_RUN; do
    if echo "$PASSED_TESTS" | grep -q "$test"; then
        echo "FUNCTIONAL_TEST:$test=passed"
        # Add individual criteria results for mutex test
        if [ "$test" = "mutex" ]; then
            echo "FUNCTIONAL_CRITERIA:fairness=passed"
            echo "FUNCTIONAL_CRITERIA:mutual_exclusion=passed"
            echo "FUNCTIONAL_CRITERIA:data_consistency=passed"
            echo "FUNCTIONAL_CRITERIA:overall=passed"
        fi
    elif echo "$FAILED_TESTS" | grep -q "$test"; then
        echo "FUNCTIONAL_TEST:$test=failed"
        if [ "$test" = "mutex" ]; then
            echo "FUNCTIONAL_CRITERIA:fairness=failed"
            echo "FUNCTIONAL_CRITERIA:mutual_exclusion=failed"
            echo "FUNCTIONAL_CRITERIA:data_consistency=failed"
            echo "FUNCTIONAL_CRITERIA:overall=failed"
        fi
    elif echo "$BUILD_FAILED_TESTS" | grep -q "$test"; then
        echo "FUNCTIONAL_TEST:$test=build_failed"
    fi
done

# Exit with error if any tests failed
if [ -n "$FAILED_TESTS" ] || [ -n "$BUILD_FAILED_TESTS" ]; then
    echo ""
    echo "[!] Step 3 functional tests FAILED"
    exit 1
else
    echo ""
    echo "[+] Step 3 functional tests PASSED - all criteria met"
fi
