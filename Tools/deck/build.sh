#!/bin/bash
# Runs inside the tools/deck/Dockerfile image (see build-deck.ps1).
#   /src  = the fork checkout (read-only), built at its committed HEAD
#   /work = persistent build volume
#   /out  = where the tarball lands
set -euo pipefail

SRC=/src
WORK=/work/ppsspp
OUT=/out

git config --global --add safe.directory '*'
git config --global protocol.file.allow always

# Fresh LF checkout of HEAD; submodules come from the local checkout, not the network.
if [ ! -d "$WORK/.git" ]; then
    git clone --no-checkout "$SRC" "$WORK"
fi
cd "$WORK"
git fetch --force "$SRC" HEAD
git checkout --force FETCH_HEAD
REV=$(git rev-parse --short HEAD)

for path in ext/OpenXR-SDK ext/SPIRV-Cross ext/aemu_postoffice ext/armips ext/cpu_features ext/discord-rpc \
            ext/freetype ext/glslang ext/libchdr ext/lua ext/miniupnp ext/naett ext/nanosvg \
            ext/rapidjson ext/rcheevos ext/zstd ffmpeg assets/debugger; do
    name=$(git config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk -v p="$path" '$2==p {print $1}' | sed 's/^submodule\.//; s/\.path$//')
    git config "submodule.$name.url" "$SRC/$path"
    git submodule update --init --force "$path"
done

# Nested submodule armips needs (ext/filesystem), also from the local checkout.
(cd ext/armips
 git config submodule.ext/filesystem.url "$SRC/ext/armips/ext/filesystem"
 git submodule update --init --force ext/filesystem)

export CCACHE_DIR=/work/ccache
cmake -S . -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DUSE_SYSTEM_LIBPNG=OFF \
    -DUSE_SYSTEM_FFMPEG=OFF \
    -DUSE_DISCORD=OFF \
    -DHEADLESS=OFF -DUNITTEST=OFF
cmake --build build --target PPSSPPSDL

# Package: binary + assets + the non-base shared libs SteamOS may lack.
PKG=ppsspp-psp2i-$REV-linux-x86_64
STAGE=/work/stage/$PKG
rm -rf /work/stage && mkdir -p "$STAGE/lib"
cp build/PPSSPPSDL "$STAGE/"
cp -r build/assets "$STAGE/"
strip "$STAGE/PPSSPPSDL"
# SDL2 and SDL2_ttf, plus two libs Ubuntu's SDL2 links that SteamOS may not ship.
for lib in libSDL2-2.0.so.0 libSDL2_ttf-2.0.so.0 libdecor-0.so.0 libXss.so.1; do
    cp -L "$(ldconfig -p | awk -v l="$lib" '$1==l {print $NF; exit}')" "$STAGE/lib/"
done
# DT_RPATH, not RUNPATH: it beats LD_LIBRARY_PATH, which Steam sets to its runtime (older SDL2_ttf) for
# non-Steam shortcuts. It also applies to the bundled libs' own dependencies.
patchelf --force-rpath --set-rpath '$ORIGIN/lib' "$STAGE/PPSSPPSDL"

# Portable mode: the fork uses ./memstick when it exists (config in memstick/PSP/SYSTEM/ppsspp.ini).
mkdir -p "$STAGE/memstick/PSP/SYSTEM" "$STAGE/memstick/PSP/SAVEDATA"
chmod +x "$STAGE/PPSSPPSDL"
echo "$REV" > "$STAGE/REVISION"

mkdir -p "$OUT"
tar -C /work/stage -czf "$OUT/$PKG.tar.gz" "$PKG"
sha256sum "$OUT/$PKG.tar.gz"
echo "glibc needed: $(objdump -T "$STAGE/PPSSPPSDL" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
ldd "$STAGE/PPSSPPSDL" | grep -v '=> /lib\|linux-vdso\|ld-linux' || true
