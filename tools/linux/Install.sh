#!/bin/sh
set -eu
cd -- "$(dirname -- "$0")"
if ! sha256sum -c SHA256SUMS; then
 kdialog --error 'Engage Recomp: checksum verification failed.'
 exit 1
fi
export APPIMAGE_EXTRACT_AND_RUN=1
mkdir -p "$HOME/.local/share/icons/hicolor/256x256/apps"
cp engage.png "$HOME/.local/share/icons/hicolor/256x256/apps/engage-recomp.png"
exec python3 ./run_appimage.py ./Engage-Recomp-0.3.4-linux-x86_64.AppImage --install "$@"
