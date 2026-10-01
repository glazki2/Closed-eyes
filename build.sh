#!/usr/bin/env bash
# Сборка ghost.so под Linux x86_64 для CS2.
#
# CS2 работает в Steam Runtime 3 "sniper" (glibc 2.31). Плагин, собранный на свежем дистрибутиве
# (например, Ubuntu 24.04 с glibc 2.39), Metamod загрузить не сможет: в `meta list` будет <ERROR>.
# Поэтому собираем clang'ом с sysroot из пакетов Ubuntu 20.04 (glibc 2.31, libstdc++ из GCC 10).
#
# Нужны: git, python3, pip, clang, lld, curl, dpkg-deb, zip, binutils.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
DEPS="${DEPS:-$ROOT/.deps}"
HL2SDK_REV="${HL2SDK_REV:-22087f5}"   # hl2sdk, ветка cs2
MMS_REV="${MMS_REV:-05c5c63}"         # metamod-source 2.0 (KHook, plugin API 18)
UBUNTU_MIRROR="${UBUNTU_MIRROR:-http://archive.ubuntu.com/ubuntu}"
MAX_GLIBC="2.31"

mkdir -p "$DEPS"
cd "$DEPS"

# --- sysroot с glibc 2.31 (Ubuntu 20.04 focal) ---
SYSROOT="$DEPS/sysroot-focal"
if [ ! -f "$SYSROOT/.done" ]; then
  rm -rf "$SYSROOT" focal-debs && mkdir -p "$SYSROOT" focal-debs
  for idx in focal/main focal-updates/main focal-updates/universe; do
    curl -fsSL "$UBUNTU_MIRROR/dists/${idx%/*}/${idx#*/}/binary-amd64/Packages.gz" -o "focal-debs/$(echo $idx | tr / _).gz"
  done
  python3 - "$UBUNTU_MIRROR" > focal-debs/urls <<'EOF'
import gzip, re, sys, glob
want = {'libc6','libc6-dev','linux-libc-dev','libgcc-10-dev','libstdc++-10-dev','libgcc-s1',
        'libcrypt-dev','libcrypt1','libstdc++6','gcc-10-base','libuuid1'}
best = {}
for f in sorted(glob.glob('focal-debs/*.gz')):  # focal_main, focal-updates_* (updates win)
    for blk in gzip.open(f, 'rt', errors='ignore').read().split('\n\n'):
        m = re.search(r'^Package: (\S+)$', blk, re.M)
        if m and m.group(1) in want:
            best[m.group(1)] = re.search(r'^Filename: (\S+)', blk, re.M).group(1)
missing = want - best.keys()
if missing:
    sys.exit('missing packages: %s' % missing)
for fn in best.values():
    print(sys.argv[1] + '/' + fn)
EOF
  (cd focal-debs && xargs -n1 curl -fsSLO < urls)
  for d in focal-debs/*.deb; do dpkg-deb -x "$d" "$SYSROOT"; done
  # absolute symlinks (libdl.so -> /lib/...) must point inside the sysroot
  (cd "$SYSROOT" && find . -type l | while read -r l; do
     t="$(readlink "$l")"; case "$t" in /*) ln -sfn "$SYSROOT$t" "$l";; esac; done)
  touch "$SYSROOT/.done"
fi

mkdir -p "$DEPS/toolchain"
cat > "$DEPS/toolchain/clang" <<EOF
#!/bin/sh
exec clang --sysroot=$SYSROOT --gcc-toolchain=$SYSROOT/usr "\$@"
EOF
cat > "$DEPS/toolchain/clang++" <<EOF
#!/bin/sh
exec clang++ --sysroot=$SYSROOT --gcc-toolchain=$SYSROOT/usr "\$@"
EOF
chmod +x "$DEPS/toolchain/clang" "$DEPS/toolchain/clang++"

# --- hl2sdk, Metamod, AMBuild ---
if [ ! -d hl2sdk-cs2 ]; then
  git clone --filter=blob:none -b cs2 https://github.com/alliedmodders/hl2sdk hl2sdk-cs2
fi
git -C hl2sdk-cs2 fetch -q origin cs2 || true
git -C hl2sdk-cs2 checkout -q "$HL2SDK_REV"

if [ ! -d metamod-source ]; then
  git clone --filter=blob:none https://github.com/alliedmodders/metamod-source metamod-source
fi
git -C metamod-source fetch -q origin || true
git -C metamod-source checkout -q "$MMS_REV"
git -C metamod-source submodule update --init --depth 1 -q

if ! python3 -c "import ambuild2" 2>/dev/null; then
  [ -d ambuild ] || git clone -q --depth 1 https://github.com/alliedmodders/ambuild ambuild
  pip install ./ambuild
fi

# --- сборка ---
rm -rf "$ROOT/build"
mkdir -p "$ROOT/build"
cd "$ROOT/build"
CC="$DEPS/toolchain/clang" CXX="$DEPS/toolchain/clang++" python3 "$ROOT/ghost/configure.py" \
  --hl2sdk-root "$DEPS" \
  --mms_path "$DEPS/metamod-source" \
  --hl2sdk-manifests "$DEPS/metamod-source/hl2sdk-manifests" \
  --sdks cs2 --targets x86_64 --enable-optimize
ambuild

cd package/cs2
SO=addons/ghost/bin/linuxsteamrt64/ghost.so
strip --strip-debug "$SO"

# --- проверка: не требовать glibc новее, чем в Steam Runtime sniper ---
NEED="$(objdump -T "$SO" | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -Vu | tail -1)"
if [ "$(printf '%s\n%s\n' "$NEED" "$MAX_GLIBC" | sort -V | tail -1)" != "$MAX_GLIBC" ]; then
  echo "ОШИБКА: ghost.so требует GLIBC_$NEED, а в CS2 (Steam Runtime sniper) только $MAX_GLIBC" >&2
  exit 1
fi
if objdump -T "$SO" | grep -qE 'GLIBCXX_|CXXABI_'; then
  echo "ОШИБКА: ghost.so зависит от системного libstdc++, нужен -static-libstdc++" >&2
  exit 1
fi
echo "glibc: максимум GLIBC_$NEED (нужно <= $MAX_GLIBC) — OK"

rm -f "$ROOT/build/ghost-linux-x86_64.zip"
zip -qr "$ROOT/build/ghost-linux-x86_64.zip" addons
echo "Готово: $ROOT/build/ghost-linux-x86_64.zip"
