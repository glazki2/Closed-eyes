#!/usr/bin/env bash
# Сборка ghost.so под Linux x86_64 с теми же версиями зависимостей, что и release/ghost-linux-x86_64.zip.
# Нужны: git, python3, pip, clang, zip.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
DEPS="${DEPS:-$ROOT/.deps}"
HL2SDK_REV="${HL2SDK_REV:-22087f5}"   # alliedmodders/hl2sdk, ветка cs2
MMS_REV="${MMS_REV:-7ec0f16}"         # alliedmodders/metamod-source, последний коммит с SourceHook (до KHook)

mkdir -p "$DEPS"
cd "$DEPS"

if [ ! -d hl2sdk-cs2 ]; then
  git clone --filter=blob:none -b cs2 https://github.com/alliedmodders/hl2sdk hl2sdk-cs2
fi
git -C hl2sdk-cs2 fetch -q origin cs2 || true
git -C hl2sdk-cs2 checkout -q "$HL2SDK_REV"

if [ ! -d metamod-source ]; then
  git clone --filter=blob:none https://github.com/alliedmodders/metamod-source metamod-source
fi
git -C metamod-source checkout -q "$MMS_REV"

if ! python3 -c "import ambuild2" 2>/dev/null; then
  [ -d ambuild ] || git clone -q --depth 1 https://github.com/alliedmodders/ambuild ambuild
  pip install ./ambuild
fi

rm -rf "$ROOT/build"
mkdir -p "$ROOT/build"
cd "$ROOT/build"
python3 "$ROOT/ghost/configure.py" \
  --hl2sdk-root "$DEPS" \
  --mms_path "$DEPS/metamod-source" \
  --hl2sdk-manifests "$ROOT/ghost/hl2sdk-manifests" \
  --sdks cs2 --targets x86_64 --enable-optimize
ambuild

cd package
mkdir -p addons/configs
cp "$ROOT/ghost/configs/ghost.ini" addons/configs/
strip --strip-debug addons/ghost/ghost.so
rm -f "$ROOT/build/ghost-linux-x86_64.zip"
zip -qr "$ROOT/build/ghost-linux-x86_64.zip" addons
echo "Готово: $ROOT/build/ghost-linux-x86_64.zip"
