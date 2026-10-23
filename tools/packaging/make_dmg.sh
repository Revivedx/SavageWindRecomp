#!/bin/bash
# macOS release disk image: build/dist/Savage Wind-<release tag>-macOS.dmg holding "Savage Wind.app" next to an
# Applications shortcut (drag the app onto it to install - how Mac users expect to install). Run after `make app`.
set -e
cd "$(dirname "$0")/../.."
APP="build/Savage Wind.app"
[ -d "$APP" ] || { echo "no $APP - run make app first" >&2; exit 1; }
VERSION=$(sed -n 's/^#define SAVAGEWIND_VERSION "\(.*\)"/\1/p' runtime/ui/launcher.mm)  # (the release tag)
OUT="build/dist/Savage Wind-$VERSION-macOS.dmg"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
ditto "$APP" "$STAGE/Savage Wind.app"
ln -s /Applications "$STAGE/Applications"
mkdir -p build/dist
rm -f "$OUT"
hdiutil create -volname "Savage Wind $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -imagekey zlib-level=9 \
    -ov "$OUT" >/dev/null
echo "$OUT ($(du -h "$OUT" | cut -f1))"
