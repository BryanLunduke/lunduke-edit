#!/bin/sh
# Build lunduke-edit_0.9.1-1_amd64.deb into packaging/debs/ (overlay apt only).
# Does NOT seed lcos-live-06/config/packages.chroot.
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
VERSION="0.9.1-1"
PKGNAME="lunduke-edit_${VERSION}_amd64"
BUILD="$ROOT/build-deb"
DEST="$ROOT/packaging/src/lunduke-edit"
DEB_DIR="$ROOT/packaging/debs"

cd "$ROOT"

rm -rf "$BUILD"
meson setup "$BUILD" --prefix=/usr --buildtype=release -Dstrip=true
meson compile -C "$BUILD"
# One session bus for the suite. Editors inherit it, and the bus exits
# with this command instead of leaving a dbus-daemon per window.
if [ -z "${DISPLAY:-}" ]; then
  xvfb-run -a dbus-run-session -- meson test -C "$BUILD"
else
  dbus-run-session -- meson test -C "$BUILD"
fi

rm -rf "$DEST"
meson install -C "$BUILD" --destdir "$DEST"

# Installed, stripped binary. Meson test production-binary scans the build
# copy; this scans the copy that goes into the deb.
"$ROOT/tests/check-production-binary.sh" "$DEST/usr/bin/lunduke-edit"

mkdir -p "$DEST/debian"
cp "$ROOT/debian/control" "$DEST/debian/control"

SHLIBS="$(
  cd "$DEST"
  dpkg-shlibdeps --ignore-missing-info -O \
    -e usr/bin/lunduke-edit
)"
SHLIBS_DEPS="${SHLIBS#shlibs:Depends=}"

SIZE="$(du -sk "$DEST/usr" | awk '{print $1}')"

mkdir -p "$DEST/DEBIAN"
cat > "$DEST/DEBIAN/control" << CTRL
Package: lunduke-edit
Version: ${VERSION}
Section: editors
Priority: optional
Architecture: amd64
Installed-Size: ${SIZE}
Maintainer: LCOS <lcos@lunduke.com>
Homepage: https://lunduke.com
Depends: ${SHLIBS_DEPS}, desktop-file-utils, gtk-update-icon-cache
Description: Lunduke Edit, a light GTK3 text editor for Linux X11
 Lunduke Edit is a classic menubar text editor for the Lunduke Computer
 Operating System. Look and feel: Windows 95 Notepad, Macintosh SimpleText,
 and BBEdit Lite — with GtkSourceView undo, Find & Replace, and Print.
CTRL

cat > "$DEST/DEBIAN/postinst" << 'POST'
#!/bin/sh
set -e
if [ "$1" = "configure" ]; then
  if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications >/dev/null 2>&1 || true
  fi
  if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q /usr/share/icons/hicolor >/dev/null 2>&1 || true
  fi
fi
exit 0
POST
chmod 0755 "$DEST/DEBIAN/postinst"

(
  cd "$DEST"
  find usr -type f -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums
)

rm -rf "$DEST/debian"

mkdir -p "$DEB_DIR"
fakeroot dpkg-deb --root-owner-group --build "$DEST" "$DEB_DIR/${PKGNAME}.deb"

echo "built $DEB_DIR/${PKGNAME}.deb"
