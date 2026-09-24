# Lunduke Edit application icon

Ship Bob’s app icon as hicolor PNGs (same layout as Lunduke Paint):

```
data/icons/hicolor/<size>x<size>/apps/org.lunduke.LundukeEdit.png
```

Sizes expected (meson installs any that exist): 16, 22, 24, 32, 48, 64, 96, 128, 256, 512.

Do **not** add a scalable app SVG — GTK prefers SVG over PNG and would ignore the rasters.

Until these files are present, the desktop entry still uses `Icon=org.lunduke.LundukeEdit`; the window/About dialog falls back to the freedesktop name `accessories-text-editor` when the theme has no Edit icon.
