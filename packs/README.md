# Resource packs

## ice-cream-sandwich

Renames the book to "Ice Cream Sandwich".

Build it with `python packs/build.py`, then drop
`packs/ice-cream-sandwich.zip` into `%appdata%\.minecraft\resourcepacks` and
enable it in Options -> Resource Packs.

**This is not a code change, and that is the interesting part.** An item's
display name is not in the game's code at all -- it is one string in
`assets/minecraft/lang/en_us.json`:

    "item.minecraft.book": "Book"

A resource pack supplying that key overrides the jar's copy. Nothing is
patched, nothing is recompiled, and it survives updates.

Three things here were read out of the game rather than remembered: the key
itself, the asset path, and `pack_format: 34`, which is what 1.21.1's own
`version.json` reports as `pack_version.resource`. The pack has not been
loaded in a running client -- that part is yours to confirm.
