#!/bin/bash
set -e

# Simple TOML Report Printer
# Prints test summary in clean, readable format

TOML_FILE=${1:-test-summary.toml}

if [ ! -f "$TOML_FILE" ]; then
    echo "Error: TOML file not found: $TOML_FILE"
    echo "Usage: $0 [toml_file]"
    exit 1
fi

echo "========================================="
echo "Linmo CI Test Report"
echo "========================================="
echo ""

cat "$TOML_FILE"

echo ""
echo "========================================="
echo "Report generated from: $TOML_FILE"
echo "========================================="
