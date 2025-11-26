#!/bin/bash

# Strict error handling
set -euo pipefail

# Configuration
TIMEOUT=30
TOOLCHAIN_TYPE=${TOOLCHAIN_TYPE:-gnu}

# Define functional tests and their expected PASS criteria
declare -A FUNCTIONAL_TESTS
FUNCTIONAL_TESTS["mutex"]="Fairness: PASS,Mutual Exclusion: PASS,Data Consistency: PASS,Overall: PASS"
FUNCTIONAL_TESTS["semaphore"]="Overall: PASS"
FUNCTIONAL_TESTS["umode"]="PASS: sys_tid() returned,PASS: sys_uptime() returned,[EXCEPTION] Illegal instruction"
#FUNCTIONAL_TESTS["test64"]="Unsigned Multiply: PASS,Unsigned Divide: PASS,Signed Multiply: PASS,Signed Divide: PASS,Left Shifts: PASS,Logical Right Shifts: PASS,Arithmetic Right Shifts: PASS,Overall: PASS"
#FUNCTIONAL_TESTS["suspend"]="Suspend: PASS,Resume: PASS,Self-Suspend: PASS,Overall: PASS"

# Add more functional tests here as they are developed
# Format: FUNCTIONAL_TESTS["app_name"]="Criterion: PASS,Criterion: PASS,..."
#
# Example entries for future functional tests:
# FUNCTIONAL_TESTS["cond"]="Producer Cycles: PASS,Consumer Cycles: PASS,Mutex Trylock: PASS,Overall: PASS"
# FUNCTIONAL_TESTS["pipes"]="Bidirectional IPC: PASS,Data Integrity: PASS,Overall: PASS"
# FUNCTIONAL_TESTS["mqueues"]="Multi-Queue Routing: PASS,Task Synchronization: PASS,Overall: PASS"

# Store detailed criteria results
declare -A CRITERIA_RESULTS

# Test a single functional test: build, run, validate criteria
# Returns: 0=passed, 1=failed, 2=build_failed, 3=unknown_test
test_functional_app() {
    local test=$1

    echo "=== Functional Test: $test ==="

    # Check if test is defined
    if [ -z "${FUNCTIONAL_TESTS[$test]}" ]; then
        echo "[!] Unknown test (not in FUNCTIONAL_TESTS)"
        return 3
    fi

    # Build phase
    echo "[+] Building..."
    make clean > /dev/null 2>&1 || true # Clean previous build artifacts (failures ignored)
    if ! make "$test" TOOLCHAIN_TYPE="$TOOLCHAIN_TYPE" > /dev/null 2>&1; then
        echo "[!] Build failed"

        # Mark all criteria as build_failed
        local expected_passes="${FUNCTIONAL_TESTS[$test]}"
        IFS=',' read -ra PASS_CRITERIA <<< "$expected_passes"
        for criteria in "${PASS_CRITERIA[@]}"; do
            local criteria_key=$(echo "$criteria" | sed 's/: PASS//g' | tr '[:upper:]' '[:lower:]' | tr ' ' '_')
            CRITERIA_RESULTS["$test:$criteria_key"]="build_failed"
        done

        return 2
    fi

    # Run phase
    echo "[+] Running (timeout: ${TIMEOUT}s)..."
    local output exit_code
    output=$(timeout ${TIMEOUT}s qemu-system-riscv32 -nographic -machine virt -bios none -kernel build/image.elf 2>&1)
    exit_code=$?

    # Debug: Show first 500 chars of output
    if [ -n "$output" ]; then
        echo "[DEBUG] Output preview (first 500 chars):"
        echo "$output" | head -c 500
        echo ""
    else
        echo "[DEBUG] No output captured from QEMU"
    fi

    # Parse expected criteria
    local expected_passes="${FUNCTIONAL_TESTS[$test]}"
    IFS=',' read -ra PASS_CRITERIA <<< "$expected_passes"

    # Check for crashes first
    # Special case: umode test expects an illegal instruction exception
    if [ "$test" != "umode" ] && echo "$output" | grep -qiE "(trap|exception|fault|panic|illegal|segfault)"; then
        echo "[!] Crash detected"

        # Mark all criteria as crashed
        for criteria in "${PASS_CRITERIA[@]}"; do
            local criteria_key=$(echo "$criteria" | sed 's/: PASS//g' | tr '[:upper:]' '[:lower:]' | tr ' ' '_')
            CRITERIA_RESULTS["$test:$criteria_key"]="crashed"
        done

        return 1
    fi

    # Check exit code
    if [ $exit_code -eq 124 ]; then
        echo "[!] Timeout (test hung)"

        # Mark all criteria as timeout
        for criteria in "${PASS_CRITERIA[@]}"; do
            local criteria_key=$(echo "$criteria" | sed 's/: PASS//g' | tr '[:upper:]' '[:lower:]' | tr ' ' '_')
            CRITERIA_RESULTS["$test:$criteria_key"]="timeout"
        done

        return 1
    elif [ $exit_code -ne 0 ]; then
        echo "[!] Exit code $exit_code"

        # Mark all criteria as failed
        for criteria in "${PASS_CRITERIA[@]}"; do
            local criteria_key=$(echo "$criteria" | sed 's/: PASS//g' | tr '[:upper:]' '[:lower:]' | tr ' ' '_')
            CRITERIA_RESULTS["$test:$criteria_key"]="failed"
        done

        return 1
    fi

    # Validate criteria
    echo "[+] Checking PASS criteria:"
    local all_passes_found=true
    local missing_passes=""

    for criteria in "${PASS_CRITERIA[@]}"; do
        local criteria_key=$(echo "$criteria" | sed 's/: PASS//g' | tr '[:upper:]' '[:lower:]' | tr ' ' '_')

        if echo "$output" | grep -qF "$criteria"; then
            echo "  ✓ Found: $criteria"
            CRITERIA_RESULTS["$test:$criteria_key"]="passed"
        else
            echo "  ✗ Missing: $criteria"
            all_passes_found=false
            missing_passes="$missing_passes '$criteria'"
            CRITERIA_RESULTS["$test:$criteria_key"]="failed"
        fi
    done

    # Determine result
    if [ "$all_passes_found" = true ]; then
        echo "[✓] All criteria passed"
        return 0
    else
        echo "[!] Missing criteria:$missing_passes"
        return 1
    fi
}

echo "[+] Linmo Functional Test Suite (Step 3)"
echo "[+] Toolchain: $TOOLCHAIN_TYPE"
echo "[+] Timeout: ${TIMEOUT}s per test"
echo ""

# Get list of tests to run
if [ $# -eq 0 ]; then
    TESTS_TO_RUN=$(echo "${!FUNCTIONAL_TESTS[@]}" | tr ' ' '\n' | sort | tr '\n' ' ')
    echo "[+] Running all functional tests: $TESTS_TO_RUN"
else
    TESTS_TO_RUN="$@"
    echo "[+] Running specified tests: $TESTS_TO_RUN"
fi

echo ""

# Track results
PASSED_TESTS=""
FAILED_TESTS=""
BUILD_FAILED_TESTS=""
TOTAL_TESTS=0

# Test each app
for test in $TESTS_TO_RUN; do
    TOTAL_TESTS=$((TOTAL_TESTS + 1))

    test_functional_app "$test"
    case $? in
        0) PASSED_TESTS="$PASSED_TESTS $test" ;;
        1) FAILED_TESTS="$FAILED_TESTS $test" ;;
        2) BUILD_FAILED_TESTS="$BUILD_FAILED_TESTS $test" ;;
        3) FAILED_TESTS="$FAILED_TESTS $test" ;;
    esac
    echo ""
done

# Summary
echo ""
echo "=== STEP 3 FUNCTIONAL TEST RESULTS ==="
echo "Total tests: $TOTAL_TESTS"
[ -n "$PASSED_TESTS" ] && echo "[✓] PASSED ($(echo $PASSED_TESTS | wc -w)):$PASSED_TESTS"
[ -n "$FAILED_TESTS" ] && echo "[!] FAILED ($(echo $FAILED_TESTS | wc -w)):$FAILED_TESTS"
[ -n "$BUILD_FAILED_TESTS" ] && echo "[!] BUILD FAILED ($(echo $BUILD_FAILED_TESTS | wc -w)):$BUILD_FAILED_TESTS"

# Parseable output
echo ""
echo "[DEBUG] About to emit parseable output for $TOTAL_TESTS tests"
echo "=== PARSEABLE_OUTPUT ==="
for test in $TESTS_TO_RUN; do
    # Output overall test result
    if echo "$PASSED_TESTS" | grep -qw "$test"; then
        echo "FUNCTIONAL_TEST:$test=passed"
    elif echo "$FAILED_TESTS" | grep -qw "$test"; then
        echo "FUNCTIONAL_TEST:$test=failed"
    elif echo "$BUILD_FAILED_TESTS" | grep -qw "$test"; then
        echo "FUNCTIONAL_TEST:$test=build_failed"
    fi

    # Output individual criteria results
    if [ -n "${FUNCTIONAL_TESTS[$test]}" ]; then
        expected_passes="${FUNCTIONAL_TESTS[$test]}"
        IFS=',' read -ra PASS_CRITERIA <<< "$expected_passes"
        for criteria in "${PASS_CRITERIA[@]}"; do
            criteria_key=$(echo "$criteria" | sed 's/: PASS//g' | tr '[:upper:]' '[:lower:]' | tr ' ' '_')

            if [ -n "${CRITERIA_RESULTS["$test:$criteria_key"]}" ]; then
                echo "FUNCTIONAL_CRITERIA:$test:$criteria_key=${CRITERIA_RESULTS["$test:$criteria_key"]}"
            fi
        done
    fi
done

echo "[DEBUG] Finished emitting parseable output"

# Exit status
if [ -n "$FAILED_TESTS" ] || [ -n "$BUILD_FAILED_TESTS" ]; then
    echo ""
    echo "[!] Step 3 functional tests FAILED"
    exit 1
else
    echo ""
    echo "[+] Step 3 functional tests PASSED"
fi
