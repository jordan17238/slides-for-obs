#!/usr/bin/env bash
#
# Build SlidesForOBS.pkg from an already-built macOS plugin bundle.
#
#   usage: build-pkg.sh <path-to-slides-for-obs.plugin> [version] [output-dir]
#
# Runs on macOS only (pkgbuild/productbuild are Apple tools). Works both in CI
# and by hand on a Mac, which makes it possible to test the installer locally
# instead of waiting on a CI round-trip.
#
# The result installs the bundle into
#   /Library/Application Support/obs-studio/plugins/
# and re-running it over an existing install replaces the bundle, so the same
# .pkg serves as both installer and updater.

set -euo pipefail

PLUGIN_PATH="${1:-}"
VERSION="${2:-0.1.0}"
OUT_DIR="${3:-$(pwd)/Output}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDENTIFIER="com.jordanhunter.slides-for-obs"
INSTALL_LOCATION="/Library/Application Support/obs-studio/plugins"

if [[ -z "$PLUGIN_PATH" ]]; then
	echo "error: no plugin bundle given" >&2
	echo "usage: $0 <path-to-slides-for-obs.plugin> [version] [output-dir]" >&2
	exit 2
fi

if [[ ! -d "$PLUGIN_PATH" ]]; then
	echo "error: '$PLUGIN_PATH' is not a directory." >&2
	echo "       A .plugin is a bundle (a folder), not a single file." >&2
	exit 2
fi

echo "=== inputs ==="
echo "plugin bundle : $PLUGIN_PATH"
echo "version       : $VERSION"
echo "output dir    : $OUT_DIR"
echo
echo "=== bundle contents (so a packaging problem is visible in the log) ==="
find "$PLUGIN_PATH" -maxdepth 3 -print
echo

# Stage the payload. pkgbuild copies everything under --root into
# --install-location, so the staging directory must contain the bundle itself
# and nothing else.
STAGING="$(mktemp -d)"
trap 'rm -rf "$STAGING"' EXIT
cp -R "$PLUGIN_PATH" "$STAGING/"

mkdir -p "$OUT_DIR"
COMPONENT_PKG="$STAGING/plugin.pkg"

echo "=== pkgbuild (component) ==="
pkgbuild \
	--root "$STAGING" \
	--identifier "$IDENTIFIER" \
	--version "$VERSION" \
	--install-location "$INSTALL_LOCATION" \
	"$COMPONENT_PKG"

echo
echo "=== productbuild (distribution + UI panes) ==="
# productbuild resolves the pkg-ref by filename against --package-path, and
# welcome/conclusion against --resources.
productbuild \
	--distribution "$HERE/distribution.xml" \
	--resources "$HERE" \
	--package-path "$STAGING" \
	"$OUT_DIR/SlidesForOBS.pkg"

echo
echo "=== done ==="
ls -lh "$OUT_DIR/SlidesForOBS.pkg"
echo
echo "NOTE: this package is not signed or notarised, so macOS Gatekeeper will"
echo "      warn on first open. Right-click the .pkg and choose Open to run it"
echo "      anyway (signing needs a paid Apple Developer account)."
