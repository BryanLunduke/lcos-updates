#!/bin/sh
# Run debian/postrm against a rewritten path in a temp directory.
# The shipped script keeps a fixed literal path and is not executed here.
set -eu

ROOT="$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)"
if [ "${1:-}" != "" ]; then
  SCRIPT=$1
else
  SCRIPT=$ROOT/debian/postrm
fi

if [ ! -f "$SCRIPT" ]; then
  echo "postrm not found: $SCRIPT" >&2
  exit 1
fi

# One rm, and it is exactly the system directory. No glob, no variable.
src_rm=$(sed -n 's/^[[:space:]]*//p' "$SCRIPT" | grep -E '^rm ' || true)
if [ "$src_rm" != "rm -rf /var/lib/lcos-updates" ]; then
  echo "shipped postrm rm is not the literal state path: [$src_rm]" >&2
  exit 1
fi
if grep -F 'LCOS_UPDATES_TEST_' "$SCRIPT" >/dev/null; then
  echo "test override leaked into postrm" >&2
  exit 1
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
case "$work" in
  /tmp/*|/var/tmp/*) ;;
  *)
    echo "unexpected temp dir: $work" >&2
    exit 1
    ;;
esac

state=$work/var/lib/lcos-updates
neighbor=$work/var/lib/lcos-updates-keep
# Test-only substitution. This copy is not the script that ships.
sed "s|/var/lib/lcos-updates|$state|g" "$SCRIPT" > "$work/postrm"
chmod 0755 "$work/postrm"

copy_rm=$(sed -n 's/^[[:space:]]*//p' "$work/postrm" | grep -E '^rm ' || true)
if [ "$copy_rm" != "rm -rf $state" ]; then
  echo "refusing to run postrm; rewritten rm is [$copy_rm]" >&2
  exit 1
fi
if grep -E '^[[:space:]]*rm -rf /var/lib/lcos-updates$' "$work/postrm" >/dev/null; then
  echo "refusing to run postrm; system path is still the rm target" >&2
  exit 1
fi

setup() {
  mkdir -p "$state" "$neighbor"
  printf 'snap\n' > "$state/last-check"
  printf 'partial\n' > "$state/last-check.tmp"
  printf 'keep\n' > "$neighbor/marker"
}

assert_kept() {
  test -d "$state"
  test -f "$state/last-check"
  test -f "$state/last-check.tmp"
  test -f "$neighbor/marker"
}

assert_purged() {
  test ! -e "$state"
  test -d "$neighbor"
  test -f "$neighbor/marker"
}

# 0.9-7 has no postrm, so the upgrade that introduces this script does not
# run one. A later upgrade runs this script as `upgrade <new-version>`.
setup
assert_kept

setup
"$work/postrm" remove
assert_kept

setup
"$work/postrm" upgrade 0.9-8
assert_kept

setup
"$work/postrm" failed-upgrade 0.9-7
assert_kept

setup
"$work/postrm" abort-upgrade 0.9-7
assert_kept

setup
"$work/postrm" abort-install
assert_kept

setup
"$work/postrm" disappear lcos-updates 0.9-8
assert_kept

setup
"$work/postrm" purge
assert_purged

# set -e must not trip when the directory is already gone.
"$work/postrm" purge
assert_purged

setup
if "$work/postrm" nosuch 2>"$work/unknown.err"; then
  echo "unknown postrm argument should fail" >&2
  exit 1
fi
if ! grep -F 'unknown argument `nosuch'"'" "$work/unknown.err" >/dev/null; then
  echo "unknown argument did not report the debhelper error" >&2
  exit 1
fi
assert_kept

echo "ok: postrm purge removes state; remove and upgrade keep it"
