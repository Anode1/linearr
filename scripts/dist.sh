#!/bin/sh
# dist.sh: build a release bundle for this platform.
#
# One archive holding the binary, the example data, the documentation and a
# checksum. Binaries are not kept in git; this makes them from a tagged commit,
# so any of them can be reproduced from the source it names.
#
#   sh scripts/dist.sh [NAME]
#
# NAME is the platform label used in the archive name (linux-x86_64 and so on).
# Without it the label is guessed from uname, which is right for a local build
# and not for a cross-compile.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

name=$1
if [ -z "$name" ]; then
    case "$(uname -s)-$(uname -m)" in
        Linux-x86_64)   name=linux-x86_64 ;;
        Linux-aarch64)  name=linux-arm64 ;;
        Darwin-arm64)   name=macos-arm64 ;;
        Darwin-x86_64)  name=macos-x86_64 ;;
        *)              name=$(uname -s)-$(uname -m) ;;
    esac
fi

bin=linearr
[ -f "$bin.exe" ] && bin=linearr.exe
[ -x "$bin" ] || { echo "build first: make" >&2; exit 1; }

# From the binary, which was stamped from the git tag. Asking the source would
# be asking a second place, and two places drift.
ver=$(./"$bin" --version 2>/dev/null | head -1 | awk '{print $2}')
[ -n "$ver" ] || { echo "cannot read the version from ./$bin --version" >&2; exit 1; }

out=dist
stage=$out/linearr-$ver-$name
rm -rf "$stage"
mkdir -p "$stage/example"

cp "$bin" "$stage/"
cp example/*.csv "$stage/example/"
cp README.md LICENSE CHANGELOG.md "$stage/" 2>/dev/null || true

# The archive: zip where it is likely to be opened by hand, tar.gz elsewhere.
( cd "$out" && rm -f "linearr-$ver-$name.tar.gz" "linearr-$ver-$name.zip"
  case "$name" in
      windows-*) zip -qr "linearr-$ver-$name.zip" "linearr-$ver-$name" ;;
      *)         tar czf "linearr-$ver-$name.tar.gz" "linearr-$ver-$name" ;;
  esac )

rm -rf "$stage"          # the archive holds it; shipping both doubles the artifact

( cd "$out" && for f in linearr-$ver-$name.tar.gz linearr-$ver-$name.zip; do
      [ -f "$f" ] || continue
      if command -v sha256sum >/dev/null 2>&1; then sha256sum "$f" > "$f.sha256"
      else shasum -a 256 "$f" > "$f.sha256"; fi
      echo "  $out/$f"
      echo "  $out/$f.sha256"
  done )
