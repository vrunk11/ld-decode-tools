#!/usr/bin/env bash
#
# check-spdx-new-files.sh - require SPDX headers on newly added source files
#
# Most of the tree predates the SPDX convention, so this checks only the files
# a change *adds*, against the base ref given as the first argument. Existing
# files are converted when they are otherwise being edited, never in a sweep.
#
# Usage: tools/check-spdx-new-files.sh [base-ref]
#   base-ref  commit to diff against. Empty, all zeros (a new branch push) or
#             unknown falls back to the merge base with origin/main.
#
# SPDX-License-Identifier: GPL-3.0-or-later
# SPDX-FileCopyrightText: 2026 ld-decode-tools contributors
#

set -euo pipefail

base="${1:-}"

if [[ -z "$base" || "$base" =~ ^0+$ ]] || ! git cat-file -e "${base}^{commit}" 2>/dev/null; then
    if git rev-parse --verify --quiet origin/main > /dev/null; then
        base="$(git merge-base HEAD origin/main)"
    else
        echo "No usable base ref; nothing to compare against, skipping."
        exit 0
    fi
fi

echo "Checking files added since ${base}"

# Project-authored sources only. Vendored code (ezpwd) and prototypes keep
# whatever header their upstream gave them.
mapfile -t added < <(
    git diff --name-only --diff-filter=A "${base}" HEAD -- \
        '*.cpp' '*.h' '*.hpp' '*.c' '*.py' '*.sh' \
        | grep -Ev '^(src/efm-decoder/libs/ezpwd/|prototypes/)' || true
)

if [[ "${#added[@]}" -eq 0 ]]; then
    echo "No new source files."
    exit 0
fi

fail=0
for f in "${added[@]}"; do
    [[ -f "$f" ]] || continue
    if ! head -n 30 "$f" | grep -q 'SPDX-License-Identifier:'; then
        echo "  missing SPDX-License-Identifier: $f"
        fail=1
    fi
done

if [[ "$fail" -ne 0 ]]; then
    echo ""
    echo "New source files must carry an SPDX header (see AGENTS.md section 5.2)."
    exit 1
fi

echo "All ${#added[@]} new source file(s) carry an SPDX header."
