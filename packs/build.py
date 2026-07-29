#!/usr/bin/env python3
"""Zip each pack folder here into a .zip Minecraft can load.

Run:  python packs/build.py

An item's display name is not code -- it is a string in
assets/minecraft/lang/en_us.json, and a resource pack overriding that key wins
over the jar's copy. So renaming the book needs no patched game and no mod
loader, which is also why it survives updates and breaks nothing.

pack_format must match the target version. 1.21.1's own version.json reports
pack_version.resource = 34; that is where the number in pack.mcmeta comes from
rather than from memory.
"""
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))


def build(folder):
    out = os.path.join(HERE, folder + ".zip")
    src = os.path.join(HERE, folder)
    if os.path.exists(out):
        os.remove(out)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _dirs, files in os.walk(src):
            for f in files:
                p = os.path.join(root, f)
                # Zip entries always use forward slashes, whatever the OS does.
                z.write(p, os.path.relpath(p, src).replace(os.sep, "/"))
    names = zipfile.ZipFile(out).namelist()
    print(f"{out}  ({len(names)} entries)")
    for n in sorted(names):
        print("   ", n)


if __name__ == "__main__":
    todo = sys.argv[1:] or [
        d for d in sorted(os.listdir(HERE))
        if os.path.isdir(os.path.join(HERE, d))
    ]
    for d in todo:
        build(d)
