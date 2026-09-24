# Lunduke Edit application icon

Bob’s app icon as hicolor PNGs (same layout as Lunduke Paint):

```
data/icons/hicolor/<size>x<size>/apps/org.lunduke.LundukeEdit.png
```

Sizes shipped (meson installs any that exist): 16, 22, 24, 32, 48, 64, 96, 128, 256, 512.

`data/icons/org.lunduke.LundukeEdit-master.png` (1024×1024) is the source master — keep it in git, but do **not** install it into hicolor or the deb.

Do **not** add a scalable app SVG — GTK prefers SVG over PNG and would ignore the rasters.

Desktop / window / About use `Icon=org.lunduke.LundukeEdit` / `set_*_icon_name`; if the theme has no match they fall back to `accessories-text-editor`.
