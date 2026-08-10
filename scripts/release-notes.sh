#!/bin/sh
# release-notes.sh: scaffold the next CHANGELOG entry from the git log.
#
# Borrowed from the kul project, where release notes are generated from
# `git log <prev>..<new>` and then curated by hand. The generation is the part
# that matters: a hand-written changelog records what somebody remembered, and
# what gets forgotten is exactly what nobody thought worth mentioning at the
# time. The curation is still required -- commit subjects are not release notes.
#
#   sh scripts/release-notes.sh 0.5.0            # print the scaffold
#   sh scripts/release-notes.sh 0.5.0 --write    # prepend it to CHANGELOG.md
#
# Then edit CHANGELOG.md: group the entries, drop the noise, and say what each
# change means for somebody using the program rather than what it did to the
# source.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

ver=$1
[ -n "$ver" ] || { echo "usage: $0 VERSION [--write]" >&2; exit 2; }
case "$ver" in v*) echo "give the version without the v: ${ver#v}" >&2; exit 2 ;; esac

prev=$(git describe --tags --abbrev=0 2>/dev/null || true)
if [ -n "$prev" ]; then range="$prev..HEAD"; else range=""; fi

{
    printf '## %s\n\n' "$ver"
    if [ -n "$prev" ]; then
        printf '_Scaffold from `git log %s`; curate before releasing._\n\n' "$range"
    else
        printf '_Scaffold from the whole history; curate before releasing._\n\n'
    fi
    # Subjects only, newest first, merges dropped: a merge subject says which
    # branch, which is never what a reader of a changelog wants.
    git log --no-merges --format='- %s' $range
    printf '\n'
} > "$root/.release-notes.tmp"

if [ "$2" = "--write" ]; then
    if grep -q "^## $ver\$" CHANGELOG.md; then
        echo "CHANGELOG.md already has a $ver section; not touching it" >&2
        rm -f "$root/.release-notes.tmp"
        exit 1
    fi
    awk -v f="$root/.release-notes.tmp" '
        NR == 1 { print; while ((getline line < f) > 0) print line; next }
        { print }' CHANGELOG.md > CHANGELOG.md.new
    mv CHANGELOG.md.new CHANGELOG.md
    echo "prepended $ver to CHANGELOG.md; curate it now"
else
    cat "$root/.release-notes.tmp"
fi
rm -f "$root/.release-notes.tmp"
