#!/bin/bash
set -e

# Unified CI Tools Script
# Combines all CI helper functionality into a single clean tool

SCRIPT_NAME=$(basename "$0")
COMMAND=${1:-help}

show_help() {
    cat << EOF
Usage: $SCRIPT_NAME <command> [options]

Commands:
  collect-data <toolchain> <test_output> [functional_output]
    Extract and store test data from CI outputs

  aggregate <results_dir> <output_file>
    Combine results from all toolchains into TOML summary

  format-comment <toml_file>
    Generate formatted PR comment from TOML

  post-comment <toml_file> <pr_number>
    Post formatted comment to PR

  print-report <toml_file>
    Print clean TOML report

Examples:
  $SCRIPT_NAME collect-data gnu "\$test_output" "\$functional_output"
  $SCRIPT_NAME aggregate all-test-results test-summary.toml
  $SCRIPT_NAME format-comment test-summary.toml
  $SCRIPT_NAME post-comment test-summary.toml 123
  $SCRIPT_NAME print-report test-summary.toml
EOF
}

# Data collection function
collect_data() {
    local toolchain=${1:-unknown}
    local test_output=${2:-}
    local functional_output=${3:-}

    if [ -z "$test_output" ]; then
        echo "Error: test_output required"
        exit 1
    fi

    mkdir -p test-results
    echo "$toolchain" > test-results/toolchain

    # Extract app data
    echo "$test_output" | grep "APP_STATUS:" | sed 's/APP_STATUS://' > test-results/apps_data || touch test-results/apps_data

    # Extract functional data
    if [ -n "$functional_output" ]; then
        echo "$functional_output" | grep "FUNCTIONAL_CRITERIA:" | sed 's/FUNCTIONAL_CRITERIA://' > test-results/functional_data || touch test-results/functional_data
    else
        touch test-results/functional_data
    fi

    # Determine exit codes
    test_exit=0
    functional_exit=0
    echo "$test_output" | grep -q "Step 2 validation FAILED" && test_exit=1
    [ -n "$functional_output" ] && echo "$functional_output" | grep -q "Step 3 functional tests FAILED" && functional_exit=1

    echo "$test_exit" > test-results/crash_exit_code
    echo "$functional_exit" > test-results/functional_exit_code

    echo "Test data collected for $toolchain toolchain"
}

# Result aggregation function
aggregate_results() {
    local results_dir=${1:-all-test-results}
    local output_file=${2:-test-summary.toml}

    if [ ! -d "$results_dir" ]; then
        echo "Error: Results directory not found: $results_dir"
        exit 1
    fi

    # Initialize status
    gnu_build="failed" gnu_crash="failed" gnu_functional="failed"
    llvm_build="failed" llvm_crash="failed" llvm_functional="failed"
    overall="failed"
    apps_data="" functional_data=""

    # Process artifacts
    for artifact_dir in "$results_dir"/test-results-*; do
        [ ! -d "$artifact_dir" ] && continue

        toolchain=$(cat "$artifact_dir/toolchain" 2>/dev/null || echo "unknown")
        crash_exit=$(cat "$artifact_dir/crash_exit_code" 2>/dev/null || echo "1")
        functional_exit=$(cat "$artifact_dir/functional_exit_code" 2>/dev/null || echo "1")

        build_status="passed"
        crash_status=$([ "$crash_exit" = "0" ] && echo "passed" || echo "failed")
        functional_status=$([ "$functional_exit" = "0" ] && echo "passed" || echo "failed")

        case "$toolchain" in
            "gnu") gnu_build="$build_status"; gnu_crash="$crash_status"; gnu_functional="$functional_status" ;;
            "llvm") llvm_build="$build_status"; llvm_crash="$crash_status"; llvm_functional="$functional_status" ;;
        esac

        # Collect data with toolchain prefix
        if [ -f "$artifact_dir/apps_data" ] && [ -s "$artifact_dir/apps_data" ]; then
            while read -r line; do
                [ -n "$line" ] && apps_data="$apps_data ${toolchain}_${line}"
            done < "$artifact_dir/apps_data"
        fi

        if [ -f "$artifact_dir/functional_data" ] && [ -s "$artifact_dir/functional_data" ]; then
            while read -r line; do
                [ -n "$line" ] && functional_data="$functional_data ${toolchain}_${line}"
            done < "$artifact_dir/functional_data"
        fi
    done

    # Overall status
    if [ "$gnu_build" = "passed" ] && [ "$gnu_crash" = "passed" ] && [ "$gnu_functional" = "passed" ] && \
       [ "$llvm_build" = "passed" ] && [ "$llvm_crash" = "passed" ] && [ "$llvm_functional" = "passed" ]; then
        overall="passed"
    fi

    apps_data=$(echo "$apps_data" | xargs)
    functional_data=$(echo "$functional_data" | xargs)

    # Generate TOML
    cat > "$output_file" << EOF
[summary]
status = "$overall"
timestamp = "$(date -Iseconds)"

[info]
architecture = "riscv32"
timeout = 5

[gnu]
build = "$gnu_build"
crash = "$gnu_crash"
functional = "$gnu_functional"

[llvm]
build = "$llvm_build"
crash = "$llvm_crash"
functional = "$llvm_functional"
EOF

    # Add apps sections
    if [ -n "$apps_data" ]; then
        echo "" >> "$output_file"
        echo "[gnu.apps]" >> "$output_file"
        echo "$apps_data" | tr ' ' '\n' | grep "^gnu_" | sed 's/gnu_//' >> "$output_file" || true
        echo "" >> "$output_file"
        echo "[llvm.apps]" >> "$output_file"
        echo "$apps_data" | tr ' ' '\n' | grep "^llvm_" | sed 's/llvm_//' >> "$output_file" || true
    fi

    # Add functional sections
    if [ -n "$functional_data" ]; then
        echo "" >> "$output_file"
        echo "[gnu.functional_tests]" >> "$output_file"
        echo "$functional_data" | tr ' ' '\n' | grep "^gnu_" | sed 's/gnu_//' >> "$output_file" || true
        echo "" >> "$output_file"
        echo "[llvm.functional_tests]" >> "$output_file"
        echo "$functional_data" | tr ' ' '\n' | grep "^llvm_" | sed 's/llvm_//' >> "$output_file" || true
    fi

    echo "Results aggregated into $output_file"
    [ "$overall" = "passed" ] && exit 0 || exit 1
}

# TOML parsing helpers
get_value() {
    local section=$1 key=$2 file=$3
    awk -v section="$section" -v key="$key" '
    BEGIN { in_section = 0 }
    /^\[.*\]/ { in_section = 0; if ($0 == "[" section "]") in_section = 1 }
    in_section && /^[a-zA-Z_]/ {
        split($0, parts, " = "); if (parts[1] == key) { gsub(/"/, "", parts[2]); print parts[2] }
    }' "$file"
}

get_section_data() {
    local section=$1 file=$2
    awk -v section="$section" '
    BEGIN { in_section = 0 }
    /^\[.*\]/ { in_section = 0; if ($0 == "[" section "]") in_section = 1 }
    in_section && /=/ && !/^\[/ { gsub(/^[ \t]+/, ""); printf "%s ", $0 }
    ' "$file"
}

get_symbol() {
    case $1 in
        "passed") echo "✅" ;; "failed") echo "❌" ;; *) echo "⚠️" ;;
    esac
}

# PR comment formatting
format_comment() {
    local toml_file=${1:-test-summary.toml}

    if [ ! -f "$toml_file" ]; then
        echo "Error: TOML file not found: $toml_file"
        exit 1
    fi

    # Extract basic info
    overall_status=$(get_value "summary" "status" "$toml_file")
    timestamp=$(get_value "summary" "timestamp" "$toml_file")
    gnu_build=$(get_value "gnu" "build" "$toml_file")
    gnu_crash=$(get_value "gnu" "crash" "$toml_file")
    gnu_functional=$(get_value "gnu" "functional" "$toml_file")
    llvm_build=$(get_value "llvm" "build" "$toml_file")
    llvm_crash=$(get_value "llvm" "crash" "$toml_file")
    llvm_functional=$(get_value "llvm" "functional" "$toml_file")

    # Generate comment
    cat << EOF
## Linmo CI Test Results

**Overall Status:** $(get_symbol "$overall_status") $overall_status
**Timestamp:** $timestamp

### Toolchain Results

| Toolchain | Build | Crash Test | Functional |
|-----------|-------|------------|------------|
| **GNU** | $(get_symbol "$gnu_build") $gnu_build | $(get_symbol "$gnu_crash") $gnu_crash | $(get_symbol "$gnu_functional") $gnu_functional |
| **LLVM** | $(get_symbol "$llvm_build") $llvm_build | $(get_symbol "$llvm_crash") $llvm_crash | $(get_symbol "$llvm_functional") $llvm_functional |
EOF

    # Apps section
    gnu_apps=$(get_section_data "gnu.apps" "$toml_file")
    llvm_apps=$(get_section_data "llvm.apps" "$toml_file")

    if [ -n "$gnu_apps" ] || [ -n "$llvm_apps" ]; then
        echo ""
        echo "### Application Tests"
        echo ""
        echo "| App | GNU | LLVM |"
        echo "|-----|-----|------|"

        all_apps=""
        [ -n "$gnu_apps" ] && all_apps="$all_apps $(echo "$gnu_apps" | tr ' ' '\n' | cut -d= -f1)"
        [ -n "$llvm_apps" ] && all_apps="$all_apps $(echo "$llvm_apps" | tr ' ' '\n' | cut -d= -f1)"

        for app in $(echo "$all_apps" | tr ' ' '\n' | sort -u); do
            gnu_status=$(echo "$gnu_apps" | tr ' ' '\n' | grep "^${app}=" | cut -d= -f2 || echo "")
            llvm_status=$(echo "$llvm_apps" | tr ' ' '\n' | grep "^${app}=" | cut -d= -f2 || echo "")
            echo "| \`$app\` | $(get_symbol "$gnu_status") $gnu_status | $(get_symbol "$llvm_status") $llvm_status |"
        done
    fi

    # Functional tests section
    gnu_functional_tests=$(get_section_data "gnu.functional_tests" "$toml_file")
    llvm_functional_tests=$(get_section_data "llvm.functional_tests" "$toml_file")

    if [ -n "$gnu_functional_tests" ] || [ -n "$llvm_functional_tests" ]; then
        echo ""
        echo "### Functional Test Details"
        echo ""
        echo "| Test | GNU | LLVM |"
        echo "|------|-----|------|"

        all_tests=""
        [ -n "$gnu_functional_tests" ] && all_tests="$all_tests $(echo "$gnu_functional_tests" | tr ' ' '\n' | cut -d= -f1)"
        [ -n "$llvm_functional_tests" ] && all_tests="$all_tests $(echo "$llvm_functional_tests" | tr ' ' '\n' | cut -d= -f1)"

        for test in $(echo "$all_tests" | tr ' ' '\n' | sort -u); do
            gnu_status=$(echo "$gnu_functional_tests" | tr ' ' '\n' | grep "^${test}=" | cut -d= -f2 || echo "")
            llvm_status=$(echo "$llvm_functional_tests" | tr ' ' '\n' | grep "^${test}=" | cut -d= -f2 || echo "")
            echo "| \`$test\` | $(get_symbol "$gnu_status") $gnu_status | $(get_symbol "$llvm_status") $llvm_status |"
        done
    fi

    echo ""
    echo "---"
    echo "*Report generated from \`$toml_file\`*"
}

# Post PR comment
post_comment() {
    local toml_file=${1:-test-summary.toml}
    local pr_number=${2:-$GITHUB_PR_NUMBER}

    if [ -z "$pr_number" ]; then
        echo "Error: PR number not provided"
        exit 1
    fi

    if [ ! -f "$toml_file" ]; then
        echo "Error: TOML file not found: $toml_file"
        exit 1
    fi

    if [ -z "$GITHUB_TOKEN" ]; then
        echo "Error: GITHUB_TOKEN environment variable not set"
        exit 1
    fi

    comment_body=$(format_comment "$toml_file")
    temp_file=$(mktemp)
    echo "$comment_body" > "$temp_file"
    gh pr comment "$pr_number" --body-file "$temp_file"
    rm -f "$temp_file"
    echo "PR comment posted successfully"
}

# Print report
print_report() {
    local toml_file=${1:-test-summary.toml}

    if [ ! -f "$toml_file" ]; then
        echo "Error: TOML file not found: $toml_file"
        exit 1
    fi

    echo "========================================="
    echo "Linmo CI Test Report"
    echo "========================================="
    echo ""
    cat "$toml_file"
    echo ""
    echo "========================================="
    echo "Report generated from: $toml_file"
    echo "========================================="
}

# Main command dispatcher
case "$COMMAND" in
    "collect-data")
        shift
        collect_data "$@"
        ;;
    "aggregate")
        shift
        aggregate_results "$@"
        ;;
    "format-comment")
        shift
        format_comment "$@"
        ;;
    "post-comment")
        shift
        post_comment "$@"
        ;;
    "print-report")
        shift
        print_report "$@"
        ;;
    "help"|"--help"|"-h")
        show_help
        ;;
    *)
        echo "Error: Unknown command '$COMMAND'"
        echo "Use '$SCRIPT_NAME help' for usage information"
        exit 1
        ;;
esac
