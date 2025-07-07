#!/bin/bash
set -e

# Aggregate Results Script
# Combines test results from all toolchains and generates final summary

RESULTS_DIR=${1:-all-test-results}
OUTPUT_FILE=${2:-test-summary.toml}

if [ ! -d "$RESULTS_DIR" ]; then
    echo "Error: Results directory not found: $RESULTS_DIR"
    exit 1
fi

# Initialize status variables
GNU_BUILD="failed"
GNU_CRASH="failed"
GNU_FUNCTIONAL="failed"
LLVM_BUILD="failed"
LLVM_CRASH="failed"
LLVM_FUNCTIONAL="failed"
OVERALL="failed"

# Collect data from artifacts
APPS_DATA=""
FUNCTIONAL_DATA=""

for artifact_dir in "$RESULTS_DIR"/test-results-*; do
    if [ ! -d "$artifact_dir" ]; then
        continue
    fi

    # Read toolchain type
    TOOLCHAIN=$(cat "$artifact_dir/toolchain" 2>/dev/null || echo "unknown")

    # Read exit codes
    CRASH_EXIT=$(cat "$artifact_dir/crash_exit_code" 2>/dev/null || echo "1")
    FUNCTIONAL_EXIT=$(cat "$artifact_dir/functional_exit_code" 2>/dev/null || echo "1")

    # Determine status based on exit codes
    BUILD_STATUS="passed"  # Assume build passed if we got this far
    CRASH_STATUS=$([ "$CRASH_EXIT" = "0" ] && echo "passed" || echo "failed")
    FUNCTIONAL_STATUS=$([ "$FUNCTIONAL_EXIT" = "0" ] && echo "passed" || echo "failed")

    # Assign to toolchain variables
    case "$TOOLCHAIN" in
        "gnu")
            GNU_BUILD="$BUILD_STATUS"
            GNU_CRASH="$CRASH_STATUS"
            GNU_FUNCTIONAL="$FUNCTIONAL_STATUS"
            ;;
        "llvm")
            LLVM_BUILD="$BUILD_STATUS"
            LLVM_CRASH="$CRASH_STATUS"
            LLVM_FUNCTIONAL="$FUNCTIONAL_STATUS"
            ;;
    esac

    # Collect apps and functional data with toolchain prefix
    if [ -f "$artifact_dir/apps_data" ] && [ -s "$artifact_dir/apps_data" ]; then
        while read -r line; do
            if [ -n "$line" ]; then
                APPS_DATA="$APPS_DATA ${TOOLCHAIN}_${line}"
            fi
        done < "$artifact_dir/apps_data"
    fi

    if [ -f "$artifact_dir/functional_data" ] && [ -s "$artifact_dir/functional_data" ]; then
        while read -r line; do
            if [ -n "$line" ]; then
                FUNCTIONAL_DATA="$FUNCTIONAL_DATA ${TOOLCHAIN}_${line}"
            fi
        done < "$artifact_dir/functional_data"
    fi
done

# Determine overall status
if [ "$GNU_BUILD" = "passed" ] && [ "$GNU_CRASH" = "passed" ] && [ "$GNU_FUNCTIONAL" = "passed" ] && \
   [ "$LLVM_BUILD" = "passed" ] && [ "$LLVM_CRASH" = "passed" ] && [ "$LLVM_FUNCTIONAL" = "passed" ]; then
    OVERALL="passed"
fi

# Clean data (keep duplicates since they're from different toolchains)
APPS_DATA=$(echo "$APPS_DATA" | xargs)
FUNCTIONAL_DATA=$(echo "$FUNCTIONAL_DATA" | xargs)

# Generate summary using existing script
.ci/generate-test-summary.sh \
    --gnu-build "$([ "$GNU_BUILD" = "passed" ] && echo "✅ PASSED" || echo "❌ FAILED")" \
    --llvm-build "$([ "$LLVM_BUILD" = "passed" ] && echo "✅ PASSED" || echo "❌ FAILED")" \
    --gnu-crash "$([ "$GNU_CRASH" = "passed" ] && echo "✅ PASSED" || echo "❌ FAILED")" \
    --llvm-crash "$([ "$LLVM_CRASH" = "passed" ] && echo "✅ PASSED" || echo "❌ FAILED")" \
    --gnu-functional "$([ "$GNU_FUNCTIONAL" = "passed" ] && echo "✅ PASSED" || echo "❌ FAILED")" \
    --llvm-functional "$([ "$LLVM_FUNCTIONAL" = "passed" ] && echo "✅ PASSED" || echo "❌ FAILED")" \
    --overall-status "$([ "$OVERALL" = "passed" ] && echo "✅ PASSED" || echo "❌ FAILED")" \
    --apps-data "$APPS_DATA" \
    --functional-data "$FUNCTIONAL_DATA" \
    --output-file "$OUTPUT_FILE"

echo "Results aggregated into $OUTPUT_FILE"

# Exit with overall status
[ "$OVERALL" = "passed" ] && exit 0 || exit 1
