#!/bin/sh
# Build lcos-updates_0.9-10_amd64.deb into packaging/debs/ (repo-local).
# Does NOT seed lcos-live-06 or lcos-live-07.
set -eu

# meson test includes the GTK window suite. With no display, re-exec once
# under Xvfb so the package build is headless. xvfb and xauth are
# Build-Depends only; they are not installed with the package.
if [ -z "${DISPLAY:-}" ] && [ -z "${LCOS_UPDATES_UNDER_XVFB:-}" ]; then
  export LCOS_UPDATES_UNDER_XVFB=1
  exec xvfb-run -a "$0" "$@"
fi

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
VERSION="0.9-10"
PKGNAME="lcos-updates_${VERSION}_amd64"
BUILD="$ROOT/build"
DEST="$ROOT/packaging/src/lcos-updates"
DEB_DIR="$ROOT/packaging/debs"

cd "$ROOT"

rm -rf "$BUILD"
meson setup "$BUILD" --prefix=/usr --buildtype=release -Dstrip=true
meson compile -C "$BUILD"
meson test -C "$BUILD" --print-errorlogs

rm -rf "$DEST"
meson install -C "$BUILD" --destdir "$DEST"

mkdir -p "$DEST/debian"
cp "$ROOT/debian/control" "$DEST/debian/control"

# dpkg-shlibdeps needs debian/control in cwd.
SHLIBS="$(
  cd "$DEST"
  dpkg-shlibdeps --ignore-missing-info -O \
    -e usr/bin/lcos-updates \
    -e usr/libexec/lcos-updates-helper
)"
# shlibs:Depends=foo, bar
SHLIBS_DEPS="${SHLIBS#shlibs:Depends=}"

# Installed-Size is KiB.
SIZE="$(du -sk "$DEST/usr" | awk '{print $1}')"

mkdir -p "$DEST/DEBIAN"
cat > "$DEST/DEBIAN/control" << CTRL
Package: lcos-updates
Version: ${VERSION}
Section: admin
Priority: optional
Architecture: amd64
Installed-Size: ${SIZE}
Maintainer: LCOS <lcos@lunduke.com>
Homepage: https://lunduke.com
Depends: ${SHLIBS_DEPS}, polkitd, pkexec, apt, desktop-file-utils
Description: Check for updates from Devuan and LCOS
 A small GTK3 tool that checks for and installs apt upgrades
 (not dist-upgrade) via a polkit helper. No daemon, no tray.
CTRL

cat > "$DEST/DEBIAN/postinst" << 'POST'
#!/bin/sh
set -e
if [ "$1" = configure ]; then
  update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
fi
exit 0
POST
chmod 0755 "$DEST/DEBIAN/postinst"

# debian/postrm is the purge script. Installed like postinst: executable,
# in the control archive, not in the data md5sums.
cp "$ROOT/debian/postrm" "$DEST/DEBIAN/postrm"
chmod 0755 "$DEST/DEBIAN/postrm"

(
  cd "$DEST"
  find usr -type f -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums
)

rm -rf "$DEST/debian"

mkdir -p "$DEB_DIR"
fakeroot dpkg-deb --root-owner-group --build "$DEST" "$DEB_DIR/${PKGNAME}.deb"

# The built deb must contain the postrm, and that copy must be the script
# the purge test runs (literal path, purge-only).
CTRL_TMP="$(mktemp -d)"
dpkg-deb -I "$DEB_DIR/${PKGNAME}.deb" > "$CTRL_TMP/info"
# dpkg-deb -I prints " *  postrm               #!/bin/sh", not a bare name.
if ! grep -E '[[:space:]]postrm[[:space:]]' "$CTRL_TMP/info" >/dev/null; then
  echo "built deb control archive has no postrm" >&2
  cat "$CTRL_TMP/info" >&2
  exit 1
fi
dpkg-deb -e "$DEB_DIR/${PKGNAME}.deb" "$CTRL_TMP/DEBIAN"
if [ ! -f "$CTRL_TMP/DEBIAN/postrm" ] || [ ! -x "$CTRL_TMP/DEBIAN/postrm" ]; then
  echo "built deb is missing an executable postrm" >&2
  exit 1
fi
if ! cmp -s "$ROOT/debian/postrm" "$CTRL_TMP/DEBIAN/postrm"; then
  echo "deb postrm does not match debian/postrm" >&2
  exit 1
fi
sh "$ROOT/tests/test-postrm.sh" "$CTRL_TMP/DEBIAN/postrm"
rm -rf "$CTRL_TMP"

echo "built $DEB_DIR/${PKGNAME}.deb"
