#!/usr/bin/env sh
# Fetch the pinned Go SDK into a repo-local, gitignored directory so every build
# uses a tree-local Go -- never the globally installed one. The only host
# prerequisites are curl, tar, and a sha256 tool (shasum or sha256sum); NO
# pre-existing Go is required. Idempotent: a no-op once the right version is in
# place.
set -eu

VER=1.25.0
HERE=$(cd "$(dirname "$0")/.." && pwd)   # helpers/
DEST="$HERE/.toolchain"
GO="$DEST/go/bin/go"

if [ -x "$GO" ] && "$GO" version 2>/dev/null | grep -q "go$VER "; then
  exit 0
fi

os=$(uname -s | tr 'A-Z' 'a-z')
case "$os" in
  darwin | linux) ;;
  *) echo "bootstrap-go: unsupported OS: $os" >&2; exit 1 ;;
esac
arch=$(uname -m)
case "$arch" in
  x86_64 | amd64) arch=amd64 ;;
  arm64 | aarch64) arch=arm64 ;;
  *) echo "bootstrap-go: unsupported arch: $arch" >&2; exit 1 ;;
esac
plat="$os-$arch"

# Pinned SHA256 for go$VER (from https://go.dev/dl).
case "$plat" in
  darwin-amd64) sha=5bd60e823037062c2307c71e8111809865116714d6f6b410597cf5075dfd80ef ;;
  darwin-arm64) sha=544932844156d8172f7a28f77f2ac9c15a23046698b6243f633b0a0b00c0749c ;;
  linux-amd64)  sha=2852af0cb20a13139b3448992e69b868e50ed0f8a1e5940ee1de9e19a123b613 ;;
  linux-arm64)  sha=05de75d6994a2783699815ee553bd5a9327d8b79991de36e38b66862782f54ae ;;
  *) echo "bootstrap-go: no pinned checksum for $plat" >&2; exit 1 ;;
esac

tarball="go$VER.$plat.tar.gz"
url="https://go.dev/dl/$tarball"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM

echo "bootstrap-go: downloading $url"
curl -fsSL "$url" -o "$tmp/$tarball"

if command -v shasum >/dev/null 2>&1; then
  got=$(shasum -a 256 "$tmp/$tarball" | awk '{print $1}')
elif command -v sha256sum >/dev/null 2>&1; then
  got=$(sha256sum "$tmp/$tarball" | awk '{print $1}')
else
  echo "bootstrap-go: need shasum or sha256sum" >&2; exit 1
fi
if [ "$got" != "$sha" ]; then
  echo "bootstrap-go: checksum mismatch for $tarball" >&2
  echo "  got  $got" >&2
  echo "  want $sha" >&2
  exit 1
fi

rm -rf "$DEST/go"
mkdir -p "$DEST"
tar -C "$DEST" -xzf "$tmp/$tarball"   # extracts to $DEST/go
echo "bootstrap-go: ready -> $GO"
"$GO" version
