#!/bin/bash
# One-time setup after cloning:
#   1. fetch the submodules,
#   2. apply the PS4 patch to fallout2-ce,
#   3. overlay the OpenOrbis v0.5.4 release (headers, libs, tools) onto the toolchain submodule,
#   4. fetch OpenOrbis' PkgTool.Core dependency, OpenSSL 1.1, into deps/ if the host has none.
# Safe to run again: every step is skipped when already done.
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

TOOLCHAIN_URL="https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/releases/download/v0.5.4/toolchain-llvm-18.tar.gz"
TOOLCHAIN_SHA256="3c7cd5bb593ca74fa1c13fd59f3938dc0fc07985167f7275063019e63abe4526"
OPENSSL_URL="http://archive.ubuntu.com/ubuntu/pool/main/o/openssl/libssl1.1_1.1.1f-1ubuntu2_amd64.deb"
OPENSSL_SHA256="09ee28588a1fb5613ddc6c26a992d5a76931b3cf22c022930da413a5e580599e"

fetch() {  # fetch <url> <dest> <sha256>
  if [ ! -f "$2" ] || ! echo "$3  $2" | sha256sum -c --quiet - 2>/dev/null; then
    echo "Downloading $1"
    curl -fL --retry 3 -o "$2.part" "$1"
    mv "$2.part" "$2"
  fi
  echo "$3  $2" | sha256sum -c --quiet - || { echo "Checksum mismatch for $2" >&2; exit 1; }
}

for tool in git curl clang clang++ ld.lld python3 make; do
  command -v "$tool" >/dev/null || { echo "Missing host tool: $tool" >&2; exit 1; }
done

echo "== Submodules"
git submodule update --init

echo "== fallout2-ce PS4 patch"
PATCH="$ROOT/patches/fallout2-ce-ps4.patch"
if git -C external/fallout2-ce apply --reverse --check "$PATCH" 2>/dev/null; then
  echo "already applied"
else
  git -C external/fallout2-ce apply "$PATCH"
  echo "applied"
fi

mkdir -p deps

echo "== OpenOrbis toolchain release"
if [ -x external/PS4Toolchain/bin/linux/create-fself ]; then
  echo "already installed"
else
  fetch "$TOOLCHAIN_URL" deps/toolchain-llvm-18.tar.gz "$TOOLCHAIN_SHA256"
  tar xzf deps/toolchain-llvm-18.tar.gz --strip-components=2 -C external/PS4Toolchain
  echo "installed into external/PS4Toolchain"
fi

echo "== OpenSSL 1.1 (for PkgTool.Core)"
if ldconfig -p 2>/dev/null | grep -q 'libssl\.so\.1\.1 '; then
  echo "provided by the host"
elif [ -f deps/openssl11/usr/lib/x86_64-linux-gnu/libssl.so.1.1 ]; then
  echo "already in deps/openssl11"
elif [ -f external/PS4Toolchain/compat/openssl11/usr/lib/x86_64-linux-gnu/libssl.so.1.1 ]; then
  echo "already in external/PS4Toolchain/compat/openssl11"
else
  fetch "$OPENSSL_URL" deps/libssl1.1.deb "$OPENSSL_SHA256"
  rm -rf deps/openssl11 && mkdir -p deps/openssl11
  if command -v dpkg-deb >/dev/null; then
    dpkg-deb -x deps/libssl1.1.deb deps/openssl11
  else
    (cd deps/openssl11 && ar p ../libssl1.1.deb data.tar.xz | tar xJ)
  fi
  echo "extracted into deps/openssl11 (build.sh adds it to LD_LIBRARY_PATH)"
fi

echo
echo "Setup done. Next:"
echo "  ./scripts/stage_assets.sh /path/to/your/Fallout2   # your own game files"
echo "  ./build.sh all"
