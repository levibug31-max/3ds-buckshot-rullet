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

fetch_latest_asset() {
  # $1 = repo, $2 = grep pattern for the asset name, $3 = output file
  local repo="$1" pattern="$2" out="$3"
  echo "Resolving latest asset for $repo matching '$pattern'..."
  local url
  url=$(curl -sL "${AUTH[@]}" "https://api.github.com/repos/${repo}/releases/latest" \
        | grep -oE '"browser_download_url": *"[^"]*"' \
        | sed -E 's/.*"(https[^"]*)".*/\1/' \
        | grep -iE "$pattern" | head -n1 || true)
  if [ -z "$url" ]; then
    echo "Could not resolve asset url for $repo / $pattern" >&2
    return 1
  fi
  echo "  -> $url"
  curl -sL "${AUTH[@]}" -o "$out" "$url"
}

# ---- makerom ----
if [ ! -x "$TOOLS/makerom" ]; then
  echo "Fetching makerom..."
  fetch_latest_asset "3DSGuy/Project_CTR" "makerom.*(ubuntu|linux).*x86_64.*\.zip" "$TOOLS/makerom.zip"
  ( cd "$TOOLS" && unzip -o makerom.zip >/dev/null && rm -f makerom.zip )
  chmod +x "$TOOLS/makerom"
fi

# ---- bannertool ----
if [ ! -x "$TOOLS/bannertool" ]; then
  echo "Fetching bannertool..."
  if fetch_latest_asset "carstene1ns/3ds-bannertool" "linux.*x86_64.*\.zip" "$TOOLS/bannertool.zip"; then
    :
  else
    fetch_latest_asset "Steveice10/bannertool" "linux.*x86_64.*\.zip" "$TOOLS/bannertool.zip"
  fi
  ( cd "$TOOLS" && unzip -o bannertool.zip >/dev/null && rm -f bannertool.zip )
  # archive may nest the binary in a folder; normalize it
  if [ ! -f "$TOOLS/bannertool" ]; then
    found=$(find "$TOOLS" -type f -name bannertool | head -n1 || true)
    [ -n "$found" ] && cp "$found" "$TOOLS/bannertool"
  fi
  chmod +x "$TOOLS/bannertool"
fi

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
