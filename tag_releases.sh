#!/usr/bin/env bash
# tag_releases.sh — create annotated git tags for each versioned qi release.
#
# Run from inside the qi repository root:
#   chmod +x tag_releases.sh
#   ./tag_releases.sh
#
# The script is idempotent: it skips any tag that already exists.

set -euo pipefail

tag() {
    local hash="$1" version="$2" message="$3"
    if git rev-parse "refs/tags/$version" >/dev/null 2>&1; then
        echo "  skip  $version (already exists)"
    else
        git tag -a "$version" "$hash" -m "$message"
        echo "  tagged $version -> $hash"
    fi
}

echo "Creating annotated tags..."

tag 2dce2eb v1.0     "1.0: initial commit"
tag 8e949a0 v1.0.1   "1.0.1: add support for Delete key"
tag 82e7358 v1.0.2   "1.0.2: make find actually usable"
tag dff0541 v1.0.3   "1.0.3: add replace function"
tag 556cc14 v1.0.4   "1.0.4: add confirmation prompt when quitting with unsaved changes"
tag 596ddf8 v1.0.5   "1.0.5: add syntax highlighting"
tag 6ff3ee3 v1.0.6   "1.0.6: add help window, change command bar"
tag a7a5e88 v1.0.7   "1.0.7: modified unsaved lines are yellow"
tag 0ca4988 v1.0.9   "1.0.9: use true tabs when editing Makefiles"
tag 2559c64 v1.0.10  "1.0.10: ESC cancels commands"
tag d8e3e2c v1.0.11  "1.0.11: update messages to show precise changes"
tag 382c7c4 v1.0.12  "1.0.12: fixed word wrapping"
tag 262d9f2 v1.0.13  "1.0.13: add ^T and ^B keybinds"
tag aefa438 v1.0.14  "1.0.14: add left/right word hopping"
tag 894a184 v1.0.15  "1.0.15: modify undo system to use fewer resources"
tag 916252c v1.0.16  "1.0.16: add vim-like scrolloff"
tag d005ee5 v1.0.17  "1.0.17: fix backspace bug; display cursor position"
tag 4f2bf15 v1.0.18  "1.0.18: display cursor position; QOL flicker prevention"
tag 0d31923 v1.0.19  "1.0.19: QOL change - flicker prevention"
tag d392a64 v1.0.20  "1.0.20: add auto-save functionality"
tag d8e3e2c v1.0.21  "1.0.21: ^F is now case-insensitive" 2>/dev/null || \
    tag d392a64 v1.0.21 "1.0.21: ^F is now case-insensitive"
tag d08b804 v1.0.22  "1.0.22: add overwrite mode"
tag f2526c2 v1.0.23  "1.0.23: add margin at column 81"
tag 539c90e v1.0.24  "1.0.24: change undo to track deltas only; undo() now undoes pasted blocks"
tag e3cd153 v1.1.0   "1.1.0: added large file support and improved memory usage"
tag a5f8226 v1.1.1   "1.1.1: fixed line wrapping"
tag 52c81f9 v1.1.2   "1.1.2: improved syntax highlighting"
tag 6fa9181 v1.1.3   "1.1.3: highlight matching brackets"
tag f9c198f v1.1.4   "1.1.4-5: restored status bar info; updated undo system; added redo"
tag 4c55448 v1.1.9   "1.1.9: fixed redo"
tag bdadc80 v1.1.9.1 "Added README and LICENSE files; qi is now GPL software"
tag 2e8127c v1.1.10  "1.1.10: various bug fixes"

echo ""
echo "Done. To push all tags to GitHub:"
echo "  git push origin --tags"
