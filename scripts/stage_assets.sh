#!/bin/bash
# Usage: scripts/stage_assets.sh /path/to/Fallout2
# Stage game data from a Windows (GOG/Steam) Fallout 2 install into ./gamedata as hardlinks
# (pkg_build mis-sizes symlinks). Read-only data only: no .exe/.dll/docs/saves.
# Hardlinks need gamedata/ and the source on the same filesystem; otherwise files are copied.
set -e
[ -d "$1" ] || { echo "usage: $0 /path/to/Fallout2" >&2; exit 1; }
SRC="$(cd "$1" && pwd)"
cd "$(dirname "$0")/.."

put() {  # put <src> <dst>
  mkdir -p "$(dirname "$2")"
  ln "$1" "$2" 2>/dev/null || cp "$1" "$2"
}

# Case-insensitive lookup of a top-level file in the install.
find_top() { find "$SRC" -maxdepth 1 -type f -iname "$1" | head -n 1; }

rm -rf gamedata; mkdir gamedata
for f in master.dat critter.dat patch000.dat fallout2.cfg; do
  p="$(find_top "$f")"
  [ -n "$p" ] || { echo "missing $f in $SRC" >&2; exit 1; }
  put "$p" "gamedata/$f"
done
# High Resolution Patch config (optional, read by fallout2-ce if present).
for f in f2_res.ini f2_res.dat; do
  p="$(find_top "$f")"
  [ -n "$p" ] && put "$p" "gamedata/$f"
done
(cd "$SRC" && find data -type f ! -ipath 'data/SAVEGAME/*' 2>/dev/null) | while read -r f; do
  put "$SRC/$f" "gamedata/$f"
done
# Music (ACM) is outside data/; music_path1/2 are pointed here at runtime.
(cd "$SRC" && find sound -type f -iname '*.acm' 2>/dev/null) | while read -r f; do
  put "$SRC/$f" "gamedata/$f"
done
echo "staged $(find -L gamedata -type f | wc -l) files, $(du -shL gamedata | cut -f1)"
