#!/usr/bin/env bash
# The release zip: the build (run tools/build_armhf.sh first) packaged with mpc-vst-plugins' shared addin installer, then checked as
# the catalog will check it. Needs mpc-vst-plugins next to this repo (or MPC_VST=/path), docs/ADDINS.md there.
# Usage: tools/release.sh <version>   ->  dist/<Name>-<version>-mpc-armv7.zip
# REPO=owner/name overrides the GitHub repository written into the package.
set -euo pipefail
cd "$(dirname "$0")/.."
[ $# -eq 1 ] || { echo "usage: $0 <version>" >&2; exit 2; }
MPC_VST="${MPC_VST:-../mpc-vst-plugins}"
REPO="${REPO:-lunarscraper/mpc-addin-surface}"
python3 "$MPC_VST/tools/release_addin.py" --dir build/package --version "$1" --repo "$REPO" --license MIT \
  --about "External MIDI controllers press buttons of the built-in control surface (e.g. switch Matrix, Mixer, Track Edit); logs what the surface sends." -o dist
python3 "$MPC_VST/tools/catalog_check.py" dist/*-"$1"-mpc-armv7.zip --catalog --expect-id surface --expect-repo "$REPO"
