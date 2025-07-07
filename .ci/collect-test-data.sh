#!/bin/bash
set -e

# Collect Test Data Script
# Extracts parseable data from test outputs and stores for CI aggregation

TOOLCHAIN=${1:-unknown}
TEST_OUTPUT=${2:-}
FUNCTIONAL_OUTPUT=${3:-}

if [ -z "$TEST_OUTPUT" ]; then
    echo "Usage: $0 <toolchain> <test_output> [functional_output]"
    echo "Example: $0 gnu \"\$test_output\" \"\$functional_output\""
    exit 1
fi

# Create results directory
mkdir -p test-results

# Store basic info
echo "$TOOLCHAIN" > test-results/toolchain

# Extract and store app data from crash tests
echo "$TEST_OUTPUT" | grep "APP_STATUS:" | sed 's/APP_STATUS://' > test-results/apps_data || touch test-results/apps_data

# Extract and store functional test data
if [ -n "$FUNCTIONAL_OUTPUT" ]; then
    echo "$FUNCTIONAL_OUTPUT" | grep "FUNCTIONAL_CRITERIA:" | sed 's/FUNCTIONAL_CRITERIA://' > test-results/functional_data || touch test-results/functional_data
else
    touch test-results/functional_data
fi

# Extract exit codes for status determination
TEST_EXIT_CODE=0
FUNCTIONAL_EXIT_CODE=0

# Check if outputs indicate failure
if echo "$TEST_OUTPUT" | grep -q "Step 2 validation FAILED"; then
    TEST_EXIT_CODE=1
fi

if [ -n "$FUNCTIONAL_OUTPUT" ] && echo "$FUNCTIONAL_OUTPUT" | grep -q "Step 3 functional tests FAILED"; then
    FUNCTIONAL_EXIT_CODE=1
fi

echo "$TEST_EXIT_CODE" > test-results/crash_exit_code
echo "$FUNCTIONAL_EXIT_CODE" > test-results/functional_exit_code

echo "Test data collected for $TOOLCHAIN toolchain"
