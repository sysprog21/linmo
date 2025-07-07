#!/bin/bash
set -e

# Enhanced TOML Test Summary Generator
SUMMARY_FILE=${SUMMARY_FILE:-test-summary.toml}

# Parse arguments
GNU_BUILD_STATUS="failed"
LLVM_BUILD_STATUS="failed"
GNU_CRASH_STATUS="failed"
LLVM_CRASH_STATUS="failed"
GNU_FUNCTIONAL_STATUS="failed"
LLVM_FUNCTIONAL_STATUS="failed"
OVERALL_STATUS="failed"
APPS_DATA=""
FUNCTIONAL_DATA=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --gnu-build)
            if [[ "$2" == *"PASSED"* ]]; then GNU_BUILD_STATUS="passed"; else GNU_BUILD_STATUS="failed"; fi
            shift 2 ;;
        --llvm-build)
            if [[ "$2" == *"PASSED"* ]]; then LLVM_BUILD_STATUS="passed"; else LLVM_BUILD_STATUS="failed"; fi
            shift 2 ;;
        --gnu-crash)
            if [[ "$2" == *"PASSED"* ]]; then GNU_CRASH_STATUS="passed"; else GNU_CRASH_STATUS="failed"; fi
            shift 2 ;;
        --llvm-crash)
            if [[ "$2" == *"PASSED"* ]]; then LLVM_CRASH_STATUS="passed"; else LLVM_CRASH_STATUS="failed"; fi
            shift 2 ;;
        --gnu-functional)
            if [[ "$2" == *"PASSED"* ]]; then GNU_FUNCTIONAL_STATUS="passed"; else GNU_FUNCTIONAL_STATUS="failed"; fi
            shift 2 ;;
        --llvm-functional)
            if [[ "$2" == *"PASSED"* ]]; then LLVM_FUNCTIONAL_STATUS="passed"; else LLVM_FUNCTIONAL_STATUS="failed"; fi
            shift 2 ;;
        --overall-status)
            if [[ "$2" == *"PASSED"* ]]; then OVERALL_STATUS="passed"; else OVERALL_STATUS="failed"; fi
            shift 2 ;;
        --apps-data)
            APPS_DATA="$2"
            shift 2 ;;
        --functional-data)
            FUNCTIONAL_DATA="$2"
            shift 2 ;;
        --output-file)
            SUMMARY_FILE="$2"
            shift 2 ;;
        *) shift ;;
    esac
done

# Generate TOML summary
cat > "$SUMMARY_FILE" << EOF
[summary]
status = "$OVERALL_STATUS"
timestamp = "$(date -Iseconds)"

[info]
architecture = "riscv32"
timeout = 5

[gnu]
build = "$GNU_BUILD_STATUS"
crash = "$GNU_CRASH_STATUS"
functional = "$GNU_FUNCTIONAL_STATUS"

[llvm]
build = "$LLVM_BUILD_STATUS"
crash = "$LLVM_CRASH_STATUS"
functional = "$LLVM_FUNCTIONAL_STATUS"
EOF

# Add apps data if provided
if [ -n "$APPS_DATA" ]; then
    echo "" >> "$SUMMARY_FILE"

    # Extract GNU apps
    echo "[gnu.apps]" >> "$SUMMARY_FILE"
    echo "$APPS_DATA" | tr ' ' '\n' | grep "^gnu_" | sed 's/gnu_//' >> "$SUMMARY_FILE" || true

    echo "" >> "$SUMMARY_FILE"

    # Extract LLVM apps
    echo "[llvm.apps]" >> "$SUMMARY_FILE"
    echo "$APPS_DATA" | tr ' ' '\n' | grep "^llvm_" | sed 's/llvm_//' >> "$SUMMARY_FILE" || true
fi

# Add functional test data if provided
if [ -n "$FUNCTIONAL_DATA" ]; then
    echo "" >> "$SUMMARY_FILE"

    # Extract GNU functional tests
    echo "[gnu.functional_tests]" >> "$SUMMARY_FILE"
    echo "$FUNCTIONAL_DATA" | tr ' ' '\n' | grep "^gnu_" | sed 's/gnu_//' >> "$SUMMARY_FILE" || true

    echo "" >> "$SUMMARY_FILE"

    # Extract LLVM functional tests
    echo "[llvm.functional_tests]" >> "$SUMMARY_FILE"
    echo "$FUNCTIONAL_DATA" | tr ' ' '\n' | grep "^llvm_" | sed 's/llvm_//' >> "$SUMMARY_FILE" || true
fi

# Print minimal terminal output
echo "Linmo CI Summary:"
echo "  GNU:   build=$GNU_BUILD_STATUS crash=$GNU_CRASH_STATUS functional=$GNU_FUNCTIONAL_STATUS"
echo "  LLVM:  build=$LLVM_BUILD_STATUS crash=$LLVM_CRASH_STATUS functional=$LLVM_FUNCTIONAL_STATUS"
echo "  Overall: $OVERALL_STATUS"
echo ""
echo "Report: $SUMMARY_FILE"

# Exit with appropriate code
if [ "$OVERALL_STATUS" = "passed" ]; then
    exit 0
else
    exit 1
fi
