#!/usr/bin/env bash
#---------------------------------------------------------------------------------
# Packages the compiled .elf/.smdh into an installable .cia for FBI.
# Downloads makerom + bannertool into ./tools if they are not already present.
# Intended to run inside the devkitpro/devkitarm container (or any env with
# DEVKITARM set and the project already built via `make`).
#---------------------------------------------------------------------------------
set -euo pipefail

TARGET="BuckshotRoulette"
APP_TITLE="Buckshot Roulette"
APP_PRODUCT_CODE="CTR-P-BCKR"
APP_UNIQUE_ID="0xBC510"

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"
mkdir -p tools

TOOLS="$ROOT/tools"
export PATH="$TOOLS:$PATH"

# Optional auth header to avoid GitHub API rate limits in CI.
AUTH=()
if [ -n "${GITHUB_TOKEN:-}" ]; then
  AUTH=(-H "Authorization: Bearer ${GITHUB_TOKEN}")
fi

resolve_asset_url() {
  # $1 = repo, $2 = grep pattern for the asset name  ->  prints url
  local repo="$1" pattern="$2"
  curl -sL "${AUTH[@]}" "https://api.github.com/repos/${repo}/releases/latest" \
    | grep -oE '"browser_download_url": *"[^"]*"' \
    | sed -E 's/.*"(https[^"]*)".*/\1/' \
    | grep -iE "$pattern" | head -n1 || true
}

# Download an archive (zip or tar.gz) and extract `binname` into $TOOLS.
fetch_tool() {
  local binname="$1"; shift               # remaining args: repo:pattern pairs
  [ -x "$TOOLS/$binname" ] && return 0
  echo "Fetching $binname..."
  local url=""
  while [ $# -ge 2 ]; do
    url=$(resolve_asset_url "$1" "$2")
    if [ -n "$url" ]; then break; fi
    echo "  (no match for $1 / $2)"
    shift 2
  done
  if [ -z "$url" ]; then echo "Could not resolve $binname download" >&2; return 1; fi
  echo "  -> $url"
  local tmp; tmp=$(mktemp -d)
  case "$url" in
    *.tar.gz|*.tgz) curl -sL "${AUTH[@]}" -o "$tmp/a.tgz" "$url"; tar -xzf "$tmp/a.tgz" -C "$tmp" ;;
    *)              curl -sL "${AUTH[@]}" -o "$tmp/a.zip" "$url"; unzip -o "$tmp/a.zip" -d "$tmp" >/dev/null ;;
  esac
  local found; found=$(find "$tmp" -type f -name "$binname" | head -n1 || true)
  if [ -z "$found" ]; then echo "binary $binname not found in archive" >&2; return 1; fi
  cp "$found" "$TOOLS/$binname"
  chmod +x "$TOOLS/$binname"
  rm -rf "$tmp"
}

fetch_tool makerom \
  "3DSGuy/Project_CTR" "makerom.*(ubuntu|linux).*x86_64.*\.zip"

fetch_tool bannertool \
  "carstene1ns/3ds-bannertool" "linux.*\.(tar\.gz|zip)" \
  "Steveice10/bannertool" "linux.*\.zip"

echo "Building banner..."
"$TOOLS/bannertool" makebanner \
  -i assets/banner.png \
  -a assets/banner.wav \
  -o "$TARGET.bnr"

echo "Building CIA..."
"$TOOLS/makerom" -f cia \
  -o "$TARGET.cia" \
  -elf "$TARGET.elf" \
  -rsf app.rsf \
  -icon "$TARGET.smdh" \
  -banner "$TARGET.bnr" \
  -exefslogo \
  -target t \
  -DAPP_TITLE="$APP_TITLE" \
  -DAPP_PRODUCT_CODE="$APP_PRODUCT_CODE" \
  -DAPP_UNIQUE_ID="$APP_UNIQUE_ID"

echo "Done: $TARGET.cia"
ls -lh "$TARGET.cia" "$TARGET.3dsx" 2>/dev/null || true
