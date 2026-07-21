# Custom graphics

Drop PNGs here and the UI uses them automatically. All optional — the UI has a
clean CSS/emoji fallback for anything missing.

## Structure icons  ->  assets/structures/<name>.png
Name them by the structure id, e.g.:
  mansion.png  village.png  monument.png  desert_pyramid.png  jungle_temple.png
  igloo.png  outpost.png  ancient_city.png  ruined_portal.png  trial_chambers.png
  shipwreck.png  ocean_ruin.png  fortress.png  bastion.png  end_city.png
Recommended: square, 32-64px, transparent background. Rendered pixelated.

## Item icons  ->  assets/items/<name>.png
Name them by the item id (no "minecraft:" prefix), e.g.:
  diamond.png  emerald.png  golden_apple.png  gold_ingot.png  iron_ingot.png
  diamond_horse_armor.png  enchanted_golden_apple.png  ...

## Backdrop  ->  assets/backdrop.png  (optional)
A tiling texture used behind the header. Anything ~64px that tiles cleanly.

Nothing here is shipped with the project — these are yours to add. The tool
does not bundle Minecraft textures (those are Mojang's); provide your own or
leave the folder empty and enjoy the built-in look.
