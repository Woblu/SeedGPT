#!/usr/bin/env bash
# Turn a finished build into the folder a player downloads: the search engine,
# the web page and a private copy of Python, so nothing has to be installed.
#
#   ./package.sh <platform>     windows-x86_64 | linux-x86_64 | linux-arm64 | macos-arm64
#
# Run ./build.sh all first, with a MARCH that suits strangers' machines rather
# than this one (.github/workflows/release.yml has the exact flags). Writes
# dist/seedsmith-<version>-<platform>/ and its archive.
set -e
cd "$(dirname "$0")"

PLATFORM=${1:?usage: ./package.sh windows-x86_64|linux-x86_64|linux-arm64|macos-arm64}
VERSION=$(tr -d '[:space:]' < VERSION)
NAME="seedsmith-$VERSION-$PLATFORM"
OUT="dist/$NAME"

# Python is downloaded rather than taken from this machine, and pinned by hash:
# it goes out under this project's name, so a changed file must stop the build.
PY_RELEASE=20261026
PY_VERSION=3.12.16
case "$PLATFORM" in
  windows-x86_64) TRIPLE=x86_64-pc-windows-msvc
                  SHA=1f2ab53880fc67ea323398c6388e05af28addc99a8e389209f08a51fdff4b149 ;;
  linux-x86_64)   TRIPLE=x86_64-unknown-linux-gnu
                  SHA=ce4f6d0a6cfdb3bed6e44c2f9affd85943c76a42e25ad3a259002f31d6fb8a20 ;;
  linux-arm64)    TRIPLE=aarch64-unknown-linux-gnu
                  SHA=bb4b6392670c5d3f93876996fcaa19858a4a413a0c7720144c18b0d6f1f0db2d ;;
  macos-arm64)    TRIPLE=aarch64-apple-darwin
                  SHA=353d5f1b4fd2715e7053f12f068435d5bbad16b91e057006b5d01e0925709a7e ;;
  *) echo "unknown platform $PLATFORM" >&2; exit 2 ;;
esac

rm -rf "$OUT"
mkdir -p "$OUT/build" dist

# Only what the page runs. The checkers and research tools stay in the source.
for t in find vocab describe map lootitems locate nearest fortoverlap structoverlap \
         checkloot checkrpchest; do
  cp "build/$t.exe" "$OUT/build/"
done
cp serve.py nlparse.py ai_prompt.py VERSION LICENSE THIRD_PARTY_NOTICES.md "$OUT/"
cp packaging/README.txt "$OUT/"
cp -R ui assets "$OUT/"
mkdir -p "$OUT/tier3-java"
cp tier3-java/find_cool_stuff.py "$OUT/tier3-java/"

case "$PLATFORM" in
  windows-*) cp "packaging/Seedsmith.bat" "$OUT/" ;;
  macos-*)   cp "packaging/Seedsmith.command" "$OUT/"
             chmod +x "$OUT/Seedsmith.command" ;;
  linux-*)   cp packaging/start.sh "$OUT/"; chmod +x "$OUT/start.sh" ;;
esac

ARCHIVE="cpython-$PY_VERSION+$PY_RELEASE-$TRIPLE-install_only_stripped.tar.gz"
URL="https://github.com/astral-sh/python-build-standalone/releases/download/$PY_RELEASE/$ARCHIVE"
CACHE="dist/$ARCHIVE"
[ -f "$CACHE" ] || curl -fsSL -o "$CACHE" "$URL"
GOT=$( (sha256sum "$CACHE" 2>/dev/null || shasum -a 256 "$CACHE") | cut -d' ' -f1)
if [ "$GOT" != "$SHA" ]; then
  echo "checksum mismatch for $ARCHIVE: got $GOT" >&2
  rm -f "$CACHE"
  exit 1
fi
tar -xzf "$CACHE" -C "$OUT"
# Nothing here builds extensions or runs tests, and they are a third of the size.
find "$OUT/python" -type d \( -name test -o -name tests -o -name __pycache__ -o -name idlelib \
  -o -name tkinter -o -name turtledemo -o -name ensurepip \) -prune -exec rm -rf {} +

cd dist
case "$PLATFORM" in
  windows-*) powershell -NoProfile -Command \
               "Compress-Archive -Path '$NAME' -DestinationPath '$NAME.zip' -Force" ;;
  macos-*)   ditto -c -k --keepParent "$NAME" "$NAME.zip" ;;
  linux-*)   tar -czf "$NAME.tar.gz" "$NAME" ;;
esac
ls -l "$NAME".*
