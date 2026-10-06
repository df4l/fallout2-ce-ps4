#!/bin/bash
# Usage: ./build.sh [make target...]
#   (none)     compile only (objs)
#   eboot.bin  link + fself
#   all        full .pkg (needs ./scripts/stage_assets.sh first)
#   clean      remove build outputs
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"
export OO_PS4_TOOLCHAIN="$ROOT/external/PS4Toolchain"

if [ ! -x "$OO_PS4_TOOLCHAIN/bin/linux/create-fself" ]; then
  echo "Toolchain binaries missing: run ./scripts/setup.sh first." >&2
  exit 1
fi

# PkgTool.Core (.NET) needs OpenSSL 1.1. setup.sh fetches it into deps/ when the host lacks it.
# A copy extracted by hand into the toolchain's compat/openssl11/ (the old manual fix) works too.
for SSL_DIR in "$ROOT/deps/openssl11/usr/lib/x86_64-linux-gnu" \
               "$OO_PS4_TOOLCHAIN/compat/openssl11/usr/lib/x86_64-linux-gnu"; do
  if [ -d "$SSL_DIR" ]; then
    export LD_LIBRARY_PATH="$SSL_DIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    break
  fi
done
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1

cd "$ROOT"
make -k -j"$(nproc)" "${@:-objs}"
