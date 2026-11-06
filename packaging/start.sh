#!/bin/sh
# Start Seedsmith from the folder this script is in.
cd "$(dirname "$0")" || exit 1
exec python/bin/python3 serve.py
