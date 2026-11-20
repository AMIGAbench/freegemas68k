#!/usr/bin/env bash
# tools/build.sh [make targets...]   CPU=68080 (default) | 68060 | all
# Builds freegemas for AmigaOS in the apollocrossdev docker image, converts the
# media (host: python3 + PIL + ffmpeg) and assembles dist/Freegemas.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
IMG="${AMIGA_IMAGE:-apollocrossdev:latest}"
TC=/opt/ApolloCrossDev/Compilers/GCC-6.50-Patched/bin
JOBS="${JOBS:-$(nproc)}"
CPU="${CPU:-68080}"
[ "$CPU" = all ] && CPUS="68080 68060" || CPUS="$CPU"

# the freegemas sources (not part of this repository): fetched at the commit the
# port is made for, then the port's changes are applied
UPSTREAM_URL="https://github.com/JoseTomasTocino/freegemas.git"
UPSTREAM_COMMIT="984644f949b2c53f8e0ff6ead13d8fcf71643664"
if [ ! -f upstream/src/Game.cpp ]; then
  git clone -q "$UPSTREAM_URL" upstream && git -C upstream checkout -q "$UPSTREAM_COMMIT" || exit 1
fi
if git -C upstream apply --check ../port/amiga/upstream.patch 2>/dev/null; then
  git -C upstream apply ../port/amiga/upstream.patch && echo "[build] port/amiga/upstream.patch applied"
elif ! git -C upstream apply --check --reverse ../port/amiga/upstream.patch 2>/dev/null; then
  echo "[build] port/amiga/upstream.patch does not apply to upstream/" >&2; exit 1
fi

# as root (some NDK headers of the image are root-only); files handed back afterwards
dock() {
  docker run --rm -e HOME=/tmp -e PATH="$TC:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" \
    -v "$ROOT":/work -w /work "$IMG" "$@"
}

for c in $CPUS; do
  echo "==== CPU $c"
  dock sh -c "make -f Makefile.amiga -j$JOBS CPU=$c $*; r=\$?; chown -R $(id -u):$(id -g) build 2>/dev/null; exit \$r" \
    || { echo "==== BUILD FAILED ====" >&2; exit 1; }
done

# guard: the gcc 6.5.0b bug pattern "btst #-n,d(aX)" (tests the wrong byte)
fail=0
for c in $CPUS; do
  dis="build/amiga/disasm-$c.txt"
  dock sh -c 'for f in build/amiga/obj-'$c'/*.o; do m68k-amigaos-objdump -d $f | sed "s|^|$f: |"; done' > "$dis" 2>/dev/null
  btst=$(awk -F'\t' 'NF >= 3 { if (hit) { if ($3 ~ /^(b(eq|ne)|s(eq|ne))/) print prev; hit = 0 }
                                if ($3 ~ /^btst #-[0-9]+,-?[0-9]+\(a[0-7]\)/) { hit = 1; prev = $0 } }' "$dis" | head -5)
  [ -n "$btst" ] && { echo "[guard] btst bug pattern ($c):"; echo "$btst"; fail=1; }
done

# media and the distribution folder
if [ ! -f build/data/.stamp ] || [ -n "$(find upstream/media tools/convert_assets.py -newer build/data/.stamp -print -quit)" ]; then
  python3 -I tools/convert_assets.py upstream/media build/data/media > /dev/null || exit 1
  touch build/data/.stamp
fi
D=dist/Freegemas
mkdir -p "$D"
rsync -a --delete build/data/media/ "$D/media/"
for p in build/amiga/Freegemas080 build/amiga/Freegemas060; do [ -f "$p" ] && cp -f "$p" "$D/"; done
cp -f port/amiga/dist/* "$D/" 2>/dev/null
# the icons: the menu logo, tool types for the audio output and fullscreen
for p in Freegemas080 Freegemas060; do
  python3 -I tools/make_icon.py upstream/media/stateMainMenu/mainMenuLogo.png "$D/$p.info" \
    "AUDIO=AHI" "(AUDIO=PAULA)" "(AUDIO=OFF)" "FULLSCREEN=NO" "(LOG=YES)" > /dev/null || exit 1
done
python3 -I tools/make_icon.py --drawer upstream/media/stateMainMenu/mainMenuLogo.png dist/Freegemas.info > /dev/null || exit 1
cp -f README.md "$D/README"
cp -f LICENSE "$D/LICENSE"
ls -l "$D"
[ "$fail" = 0 ] && echo "==== BUILD OK ====" || { echo "==== GUARDS FAILED ====" >&2; exit 2; }
