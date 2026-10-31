#!/bin/sh
# Start Seedsmith from the folder this script is in.
#
# macOS marks everything in a download as quarantined, and it refuses to run
# quarantined programs that are not signed by a paid developer account. Opening
# this script with Control-click > Open is the permission; this clears the mark
# on this folder only, so the search engine and Python inside it can run.
cd "$(dirname "$0")" || exit 1
xattr -dr com.apple.quarantine . 2>/dev/null
exec python/bin/python3 serve.py
