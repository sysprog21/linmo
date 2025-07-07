#!/bin/bash
set -e

# TOML-based PR Comment Formatter
TOML_FILE=${1:-test-summary.toml}

if [ ! -f "$TOML_FILE" ]; then
    echo "Error: TOML file not found: $TOML_FILE"
    exit 1
fi

# Simple TOML parser functions
get_value() {
    local section=$1
    local key=$2
    awk -v section="$section" -v key="$key" '
    BEGIN { in_section = 0 }
    /^\[.*\]/ {
        in_section = 0
        if ($0 == "[" section "]") in_section = 1
    }
    in_section && /^[a-zA-Z_]/ {
        split($0, parts, " = ")
        if (parts[1] == key) {
            gsub(/"/, "", parts[2])
            print parts[2]
        }
    }
    ' "$TOML_FILE"
}

get_subsection_value() {
    local section=$1
    local key=$2
    awk -v section="$section" -v key="$key" '
    BEGIN { in_section = 0 }
    /^\[.*\]/ {
        in_section = 0
        if ($0 == "[" section "]") in_section = 1
    }
    in_section && /^[a-zA-Z_]/ {
        split($0, parts, " = ")
        if (parts[1] == key) {
            gsub(/"/, "", parts[2])
            print parts[2]
        }
    }
    ' "$TOML_FILE"
}

get_apps_list() {
    local toolchain=$1
    awk -v toolchain="$toolchain" '
    BEGIN { in_section = 0 }
    /^\[.*\]/ {
        in_section = 0
        if ($0 == "[" toolchain ".apps]") in_section = 1
    }
    in_section && /=/ && !/^\[/ {
        gsub(/^[ \t]+/, "")
        printf "%s ", $0
    }
    ' "$TOML_FILE"
}

get_functional_list() {
    local toolchain=$1
    awk -v toolchain="$toolchain" '
    BEGIN { in_section = 0 }
    /^\[.*\]/ {
        in_section = 0
        if ($0 == "[" toolchain ".functional_tests]") in_section = 1
    }
    in_section && /=/ && !/^\[/ {
        gsub(/^[ \t]+/, "")
        printf "%s ", $0
    }
    ' "$TOML_FILE"
}

# Extract values
OVERALL_STATUS=$(get_value "summary" "status")
TIMESTAMP=$(get_value "summary" "timestamp")

GNU_BUILD=$(get_subsection_value "gnu" "build")
GNU_CRASH=$(get_subsection_value "gnu" "crash")
GNU_FUNCTIONAL=$(get_subsection_value "gnu" "functional")

LLVM_BUILD=$(get_subsection_value "llvm" "build")
LLVM_CRASH=$(get_subsection_value "llvm" "crash")
LLVM_FUNCTIONAL=$(get_subsection_value "llvm" "functional")

GNU_APPS_LIST=$(get_apps_list "gnu")
LLVM_APPS_LIST=$(get_apps_list "llvm")
GNU_FUNCTIONAL_LIST=$(get_functional_list "gnu")
LLVM_FUNCTIONAL_LIST=$(get_functional_list "llvm")

# Status symbols
get_symbol() {
    case $1 in
        "passed") echo "✅" ;;
        "failed") echo "❌" ;;
        *) echo "⚠️" ;;
    esac
}

# Generate formatted comment
cat << EOF
## Linmo CI Test Results

**Overall Status:** $(get_symbol "$OVERALL_STATUS") $OVERALL_STATUS
**Timestamp:** $TIMESTAMP

### Toolchain Results

| Toolchain | Build | Crash Test | Functional |
|-----------|-------|------------|------------|
| **GNU** | $(get_symbol "$GNU_BUILD") $GNU_BUILD | $(get_symbol "$GNU_CRASH") $GNU_CRASH | $(get_symbol "$GNU_FUNCTIONAL") $GNU_FUNCTIONAL |
| **LLVM** | $(get_symbol "$LLVM_BUILD") $LLVM_BUILD | $(get_symbol "$LLVM_CRASH") $LLVM_CRASH | $(get_symbol "$LLVM_FUNCTIONAL") $LLVM_FUNCTIONAL |
EOF

# Add apps section if data exists
if [ -n "$GNU_APPS_LIST" ] || [ -n "$LLVM_APPS_LIST" ]; then
    echo ""
    echo "### Application Tests"
    echo ""
    echo "| App | GNU | LLVM |"
    echo "|-----|-----|------|"

    # Get all unique app names from both toolchains
    all_apps=""
    [ -n "$GNU_APPS_LIST" ] && all_apps="$all_apps $(echo "$GNU_APPS_LIST" | tr ' ' '\n' | cut -d= -f1)"
    [ -n "$LLVM_APPS_LIST" ] && all_apps="$all_apps $(echo "$LLVM_APPS_LIST" | tr ' ' '\n' | cut -d= -f1)"
    app_names=$(echo "$all_apps" | tr ' ' '\n' | sort -u)

    for app in $app_names; do
        gnu_status=""
        llvm_status=""

        # Find GNU status
        for app_data in $GNU_APPS_LIST; do
            if [[ "$app_data" == "${app}="* ]]; then
                gnu_status=$(echo "$app_data" | cut -d= -f2)
            fi
        done

        # Find LLVM status
        for app_data in $LLVM_APPS_LIST; do
            if [[ "$app_data" == "${app}="* ]]; then
                llvm_status=$(echo "$app_data" | cut -d= -f2)
            fi
        done

        echo "| \`$app\` | $(get_symbol "$gnu_status") $gnu_status | $(get_symbol "$llvm_status") $llvm_status |"
    done
fi

# Add functional tests section if data exists
if [ -n "$GNU_FUNCTIONAL_LIST" ] || [ -n "$LLVM_FUNCTIONAL_LIST" ]; then
    echo ""
    echo "### Functional Test Details"
    echo ""
    echo "| Test | GNU | LLVM |"
    echo "|------|-----|------|"

    # Get all unique test names from both toolchains
    all_tests=""
    [ -n "$GNU_FUNCTIONAL_LIST" ] && all_tests="$all_tests $(echo "$GNU_FUNCTIONAL_LIST" | tr ' ' '\n' | cut -d= -f1)"
    [ -n "$LLVM_FUNCTIONAL_LIST" ] && all_tests="$all_tests $(echo "$LLVM_FUNCTIONAL_LIST" | tr ' ' '\n' | cut -d= -f1)"
    test_names=$(echo "$all_tests" | tr ' ' '\n' | sort -u)

    for test in $test_names; do
        gnu_status=""
        llvm_status=""

        # Find GNU status
        for test_data in $GNU_FUNCTIONAL_LIST; do
            if [[ "$test_data" == "${test}="* ]]; then
                gnu_status=$(echo "$test_data" | cut -d= -f2)
            fi
        done

        # Find LLVM status
        for test_data in $LLVM_FUNCTIONAL_LIST; do
            if [[ "$test_data" == "${test}="* ]]; then
                llvm_status=$(echo "$test_data" | cut -d= -f2)
            fi
        done

        echo "| \`$test\` | $(get_symbol "$gnu_status") $gnu_status | $(get_symbol "$llvm_status") $llvm_status |"
    done
fi

# Add footer
echo ""
echo "---"
echo "*Report generated from \`test-summary.toml\`*"
