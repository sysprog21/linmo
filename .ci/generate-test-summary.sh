#!/bin/bash
set -e

# Minimal TOML Test Summary Generator
SUMMARY_FILE=${SUMMARY_FILE:-test-summary.toml}

# Parse arguments
BUILD_STATUS="failed"
CRASH_STATUS="failed"
FUNCTIONAL_STATUS="failed"
OVERALL_STATUS="failed"

while [[ $# -gt 0 ]]; do
    case $1 in
        --build-status)
            if [[ "$2" == *"PASSED"* ]]; then BUILD_STATUS="passed"; else BUILD_STATUS="failed"; fi
            shift 2 ;;
        --crash-status)
            if [[ "$2" == *"PASSED"* ]]; then CRASH_STATUS="passed"; else CRASH_STATUS="failed"; fi
            shift 2 ;;
        --functional-status)
            if [[ "$2" == *"PASSED"* ]]; then FUNCTIONAL_STATUS="passed"; else FUNCTIONAL_STATUS="failed"; fi
            shift 2 ;;
        --overall-status)
            if [[ "$2" == *"PASSED"* ]]; then OVERALL_STATUS="passed"; else OVERALL_STATUS="failed"; fi
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
toolchain = "${TOOLCHAIN_TYPE:-gnu}"

[tests]
build = "$BUILD_STATUS"
crash = "$CRASH_STATUS"
functional = "$FUNCTIONAL_STATUS"

[info]
architecture = "riscv32"
timeout = 5
EOF

# Print minimal terminal output
echo "Linmo CI Summary:"
echo "  Build:      $BUILD_STATUS"
echo "  Crash Test: $CRASH_STATUS"
echo "  Functional: $FUNCTIONAL_STATUS"
echo "  Overall:    $OVERALL_STATUS"
echo ""
echo "Report: $SUMMARY_FILE"

# Exit with appropriate code
if [ "$OVERALL_STATUS" = "passed" ]; then
    exit 0
else
    exit 1
fi
