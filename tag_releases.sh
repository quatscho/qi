#!/usr/bin/env bash
# tag_releases.sh — create annotated git tags for each versioned qi release.
#
# Run from inside the qi repository root:
#   chmod +x tag_releases.sh
#   ./tag_releases.sh
#
# The script is idempotent: it skips any tag that already exists.
# tag_releases.sh  dynamically tag version commits and generate/update CHANGELOG.md

set -euo pipefail

CHANGELOG_FILE="CHANGELOG.md"

echo "Scanning repository for version commits..."

# Find commits whose message starts with "vX.Y.Z" or "X.Y.Z" (e.g., "1.1.38: ...")
# Format output as: HASH VERSION MESSAGE
git log --reverse --grep='^[vV]\?[0-9]\+\.[0-9]\+' --format='%h %s' | while read -r hash subject; do

    # Extract the version number (e.g., "v1.1.38" or "1.1.38")
    version=$(echo "$subject" | grep -oE '^[vV]?[0-9]+(\.[0-9]+)+')

    # Standardize tag name with a 'v' prefix if missing
    if [[ ! "$version" =~ ^v ]]; then
        tag_name="v$version"
    else
        tag_name="$version"
    fi

    # 1. Create the Git Tag if it doesn't exist
    if git rev-parse "refs/tags/$tag_name" >/dev/null 2>&1; then
        echo "  [skip tag]  $tag_name already exists"
    else
        git tag -a "$tag_name" "$hash" -m "$subject"
        echo "  [tagged]    $tag_name -> $hash"
    fi

    # 2. Append to CHANGELOG.md if the entry isn't already there
    if [ -f "$CHANGELOG_FILE" ] && grep -q "$tag_name" "$CHANGELOG_FILE"; then
        echo "  [skip log]  $tag_name already in $CHANGELOG_FILE"
    else
        echo -e "## $tag_name\n- Commit \`$hash\`: $subject\n" >> "$CHANGELOG_FILE"
        echo "  [changelog] Added $tag_name to $CHANGELOG_FILE"
    fi

done

echo ""
echo "Done!"
echo "To push tags to GitHub: git push origin --tags"
