#!/bin/bash
# Run in the Ubuntu 22.04 build container after building runtime and helper.
set -euo pipefail
cd "$(dirname "$0")/../.."
L=$PWD/build/linux/x86_64
P=${DEPS:-$L/deps}
A=$L/Engage.AppDir
mkdir -p "$A/usr/bin" "$A/usr/lib" "$A/usr/share/engage" "$A/usr/share/licenses/engage-recomp"
cp "$L/game/engage-recomp" "$A/usr/bin/"
llvm-strip --strip-debug "$A/usr/bin/engage-recomp"
cp -a "$L/helper/engagegen" "$A/usr/bin/"
cp -a build/zig "$A/usr/bin/"
mkdir -p "$A/usr/bin/gamelib-src"
cp runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h tools/gamelib_entry.c runtime/hle/hle_symbols.txt "$A/usr/bin/gamelib-src/"
cp docs/icon/engage.png "$A/usr/bin/icon.png"
cp third_party/fonts/Baloo2/Baloo2-SemiBold.ttf third_party/fonts/EBGaramond/EBGaramond12-Regular.ttf "$A/usr/bin/"
cp tools/linux/app.py tools/linux/engage_build.py tools/linux/install_core.py tools/linux/processes.py "$A/usr/share/engage/"
cp tools/linux/preparation_window.py "$A/usr/share/engage/"
cp tools/linux/run_appimage.py "$A/usr/share/engage/"
cp tools/linux/Engage-AppRun "$A/AppRun"
chmod +x "$A/AppRun"
cp docs/icon/engage.png "$A/engage-recomp.png"
ln -sf engage-recomp.png "$A/.DirIcon"
cat > "$A/engage-recomp.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Engage Recomp
Exec=AppRun
Icon=engage-recomp
Categories=Game;
Terminal=false
EOF
needed() { readelf -d "$1" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; }
SYSTEM_LIBS='^(libc|libm|libdl|libpthread|librt|libresolv|libstdc\+\+|libgcc_s|libvulkan)\.so|^ld-linux'
todo=("$A/usr/bin/engage-recomp")
while [ ${#todo[@]} -gt 0 ]; do
 f=${todo[0]}; todo=("${todo[@]:1}")
 for n in $(needed "$f"); do
  [ -e "$A/usr/lib/$n" ] && continue
  if [ -e "$P/lib/$n" ]; then
   cp -L "$P/lib/$n" "$A/usr/lib/$n"
   patchelf --set-rpath '$ORIGIN' "$A/usr/lib/$n"
   todo+=("$A/usr/lib/$n")
  elif ! [[ $n =~ $SYSTEM_LIBS ]]; then echo "Missing dependency $n"; exit 1; fi
 done
done
patchelf --set-rpath '$ORIGIN/../lib' "$A/usr/bin/engage-recomp"
python3 tools/linux/fetch_corresponding_sources.py x86_64
"$L/gen-venv/bin/python" tools/linux/collect_licenses.py x86_64
cp -a "$L/licenses/." "$A/usr/share/licenses/engage-recomp/"
cp LICENSE THIRD_PARTY.md UPSTREAM.md "$A/usr/share/licenses/engage-recomp/"
python3 tools/linux/audit_engage.py "$A" --appdir
mkdir -p build/linux/src build/dist
fetch() {
 [ -f "build/linux/src/$3" ] || curl -fL --retry 3 -o "build/linux/src/$3" "$1"
 echo "$2  build/linux/src/$3" | sha256sum -c --quiet
}
fetch https://github.com/AppImage/appimagetool/releases/download/1.9.1/appimagetool-x86_64.AppImage ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 appimagetool-x86_64.AppImage
fetch https://github.com/AppImage/type2-runtime/releases/download/20251108/runtime-x86_64 2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d runtime-x86_64
chmod +x build/linux/src/appimagetool-x86_64.AppImage
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 build/linux/src/appimagetool-x86_64.AppImage --no-appstream --runtime-file build/linux/src/runtime-x86_64 "$A" build/dist/Engage-Recomp-0.1.0-linux-x86_64.new.AppImage
mv -f build/dist/Engage-Recomp-0.1.0-linux-x86_64.new.AppImage build/dist/Engage-Recomp-0.1.0-linux-x86_64.AppImage
