# Graphics

`logo.svg` is Seedsmith's logo, original to this project and under the same MIT
license as the code.

Everything else here is optional. The page falls back to emoji for anything
missing, so these folders are empty in a fresh copy, and this project ships no
Minecraft textures (those belong to Mojang).

If you want your own icons in your own copy, drop PNGs in:

- `assets/structures/<structure id>.png`, such as `mansion.png` or `village.png`
- `assets/items/<item id>.png`, such as `diamond.png` or `golden_apple.png`
- `assets/backdrop.png`, a small image that tiles, used behind the header

Square, 32 to 64 pixels, with a transparent background works best. They are drawn
without smoothing, so pixel art stays sharp.
