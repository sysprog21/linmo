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
    local subsection=$2
    local key=$3
    awk -v section="$section" -v subsection="$subsection" -v key="$key" '
    BEGIN { in_section = 0 }
    /^\[.*\]/ {
        in_section = 0
        if ($0 == "[" section "." subsection "]") in_section = 1
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
    awk '
    /^\[apps\]/ {
        # Found [apps] section, check if data is on same line
        if (length($0) > 6) {
            # Data on same line after [apps]
            data = substr($0, 7)
            gsub(/^[ \t]+/, "", data)
            split(data, pairs, " ")
            for (i in pairs) {
                if (pairs[i] ~ /=/) {
                    printf "%s ", pairs[i]
                }
            }
        }
        getline
        # Process following lines until next section
        while ($0 !~ /^\[/ && NF > 0) {
            if ($0 ~ /=/) {
                gsub(/^[ \t]+/, "")
                split($0, pairs, " ")
                for (i in pairs) {
                    if (pairs[i] ~ /=/) {
                        printf "%s ", pairs[i]
                    }
                }
            }
            if ((getline) <= 0) break
        }
    }
    ' "$TOML_FILE"
}

get_functional_list() {
    awk '
    /^\[functional_tests\]/ {
        # Found [functional_tests] section, check if data is on same line
        if (length($0) > 18) {
            # Data on same line after [functional_tests]
            data = substr($0, 19)
            gsub(/^[ \t]+/, "", data)
            split(data, pairs, " ")
            for (i in pairs) {
                if (pairs[i] ~ /=/) {
                    printf "%s ", pairs[i]
                }
            }
        }
        getline
        # Process following lines until next section
        while ($0 !~ /^\[/ && NF > 0) {
            if ($0 ~ /=/) {
                gsub(/^[ \t]+/, "")
                split($0, pairs, " ")
                for (i in pairs) {
                    if (pairs[i] ~ /=/) {
                        printf "%s ", pairs[i]
                    }
                }
            }
            if ((getline) <= 0) break
        }
    }
    ' "$TOML_FILE"
}

# Extract values
OVERALL_STATUS=$(get_value "summary" "status")
TIMESTAMP=$(get_value "summary" "timestamp")

GNU_BUILD=$(get_subsection_value "toolchains" "gnu" "build")
GNU_CRASH=$(get_subsection_value "toolchains" "gnu" "crash")
GNU_FUNCTIONAL=$(get_subsection_value "toolchains" "gnu" "functional")

LLVM_BUILD=$(get_subsection_value "toolchains" "llvm" "build")
LLVM_CRASH=$(get_subsection_value "toolchains" "llvm" "crash")
LLVM_FUNCTIONAL=$(get_subsection_value "toolchains" "llvm" "functional")

APPS_LIST=$(get_apps_list)
FUNCTIONAL_LIST=$(get_functional_list)

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
if [ -n "$APPS_LIST" ]; then
    echo ""
    echo "### Application Tests"
    echo ""
    echo "| App | Status |"
    echo "|-----|--------|"

    for app_data in $APPS_LIST; do
        if [ -n "$app_data" ] && [[ "$app_data" == *"="* ]]; then
            app=$(echo "$app_data" | cut -d= -f1)
            status=$(echo "$app_data" | cut -d= -f2)
            echo "| \`$app\` | $(get_symbol "$status") $status |"
        fi
    done
fi

# Add functional tests section if data exists
if [ -n "$FUNCTIONAL_LIST" ]; then
    echo ""
    echo "### Functional Test Details"
    echo ""
    echo "| Test | Status |"
    echo "|------|--------|"

    for test_data in $FUNCTIONAL_LIST; do
        if [ -n "$test_data" ] && [[ "$test_data" == *"="* ]]; then
            test=$(echo "$test_data" | cut -d= -f1)
            status=$(echo "$test_data" | cut -d= -f2)
            echo "| \`$test\` | $(get_symbol "$status") $status |"
        fi
    done
fi

# Add footer
echo ""
echo "---"
echo "*Report generated from \`test-summary.toml\`*"
