#!/bin/bash
set -e

# Post PR Comment Script
# Posts formatted test summary as PR comment

TOML_FILE=${1:-test-summary.toml}
PR_NUMBER=${2:-$GITHUB_PR_NUMBER}

if [ -z "$PR_NUMBER" ]; then
    echo "Error: PR number not provided"
    echo "Usage: $0 <toml_file> [pr_number]"
    echo "Or set GITHUB_PR_NUMBER environment variable"
    exit 1
fi

if [ ! -f "$TOML_FILE" ]; then
    echo "Error: TOML file not found: $TOML_FILE"
    exit 1
fi

if [ -z "$GITHUB_TOKEN" ]; then
    echo "Error: GITHUB_TOKEN environment variable not set"
    exit 1
fi

# Generate formatted comment
COMMENT_BODY=$(.ci/format-pr-comment.sh "$TOML_FILE")

# Create temporary file for comment body
TEMP_FILE=$(mktemp)
echo "$COMMENT_BODY" > "$TEMP_FILE"

# Post comment using GitHub CLI
gh pr comment "$PR_NUMBER" --body-file "$TEMP_FILE"

# Clean up
rm -f "$TEMP_FILE"

echo "PR comment posted successfully"
