# Material Overlay Authoring Guide

## What this system is

Duke-RT uses a companion-texture overlay system for material data. You mount an overlay package or loose directory, and the renderer discovers material maps under `materials/...` for the matching Duke tile.

This is a PBR-oriented pipeline. The current workflow supports:

- glow/emissive maps
- normal maps
- metallic maps
- roughness maps
- specular maps

Ambient occlusion (`AO`) is not supported in the current pipeline yet.

## Overlay layout

Check default-overlay to see the expected structure:

```text
default-overlay/
  LIGHTOVR
  materials/
    glowmaps/
      auto/
    metallic/
      auto/
    normalmaps/
      auto/
      test/
    roughness/
      auto/
    specular/
      auto/
```

For normal authoring work, use the `auto/` folders. That is the standard tile-companion lookup path that Raze supported out of the box.

Mount a loose overlay directory with `-file`, for example:

```text
build\terminal-ninja\raze.exe -file M:\Raze\default-overlay
```

## Naming rule for Duke tiles

For Duke/Build ART tiles, the file name should be the tile number with a leading `#`, padded with leading zeroes to 5 digits:

```text
#00000.png
#00138.png
#01687.png
#12345.png
```

When you add metallic, roughness, specular, or normal data for a tile, keep the same tile number and place the map in the matching material folder:

```text
materials/metallic/auto/#00138.png
materials/roughness/auto/#00138.png
materials/specular/auto/#00138.png
materials/normalmaps/auto/#00138.png
```

Glowmaps follow the same numbering rule:

```text
materials/glowmaps/auto/#00138.png
```

The important part is that the file name stays correlated to the existing Duke tile number. If the in-game tile is `138`, the overlay maps for that tile should use `#00138.png`.

## Where to get the tile numbers

If you are working from Duke Nukem 3D: 20th Anniversary World Tour, you can use the existing installed assets there as source material.

For World Tour or other Duke installs that ship `DUKE3D.GRP`, extract the `TILESXXX.ART` files from `DUKE3D.GRP` first. A practical tool for that is Group File Studio:

- Group File Studio: <https://dnr.duke4.net/utilities.php.html>

After extraction, open the `.ART` files in BAFed so you can inspect the actual tile images and tile numbers:

- BAFed / Build ART Files Editor: <https://m210.duke4.net/index.php/downloads/download/8-java/41-build-art-files-editor>

That tile number is the number you reuse in the overlay file name.

## Practical workflow

1. Find the source tile in World Tour assets or extract `TILESXXX.ART` from `DUKE3D.GRP`.
2. Open the ART file in BAFed and note the tile number.
3. Author the map you want to add.
4. Save it as `#XXXXX.png` with the tile number padded to 5 digits.
5. Place it in the matching `materials/<map-type>/auto/` folder in your mounted overlay.
6. Launch Duke-RT with that overlay mounted through `-file`.

## Quick examples

Tile `70`:

```text
materials/specular/auto/#00070.png
```

Tile `130`:

```text
materials/metallic/auto/#00130.png
materials/roughness/auto/#00130.png
materials/normalmaps/auto/#00130.png
```

Tile `205`:

```text
materials/normalmaps/auto/#00205.png
```

Those file names match the numbering pattern already used in [default-overlay](/M:/Raze/default-overlay).
