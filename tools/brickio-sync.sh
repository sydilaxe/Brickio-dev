#!/bin/sh
# Bring a brickio version into an app: its src/ copied into a folder of the app (lib/brickio by default),
# with BRICKIO_VERSION saying which. Run from the app's own repository, then build, test and commit.
#
#   tools/brickio-sync.sh v0.1.0             # a release (a tag)
#   tools/brickio-sync.sh main lib/brickio   # the latest, into lib/brickio
#
# Nothing else in the app is touched: the app chooses which modules it compiles (see brickio's
# README, "Taking it into an app"). The copy is plain files, so `git diff` shows what changed.
set -eu
ref=${1:?usage: brickio-sync.sh <tag or branch> [folder]}
dir=${2:-lib/brickio}
url=${BRICKIO_URL:-https://github.com/sydilaxe/Brickio-dev}
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
git clone -q --depth 1 --branch "$ref" "$url" "$tmp/brickio"
mkdir -p "$dir"
# brickio's files replace their old copies; any other file in the folder is left as it is
cp "$tmp"/brickio/src/*.c "$tmp"/brickio/src/*.h "$dir"/
printf '%s %s\n' "$ref" "$(git -C "$tmp/brickio" rev-parse --short HEAD)" > "$dir/BRICKIO_VERSION"
echo "brickio $(cat "$dir/BRICKIO_VERSION") in $dir"
