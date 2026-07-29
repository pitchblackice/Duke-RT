#!/usr/bin/env bash
#
# play-duke-rt.sh - launch the native Linux Duke-RT build.
#
# Any extra arguments are passed straight through to the engine, e.g.
#   ./play-duke-rt.sh -map E1L1          start directly on a level
#   ./play-duke-rt.sh +set vid_fps 1     show the framerate counter
#
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
RAZE="$ROOT/build/raze"
OVERLAY="$HOME/Downloads/Duke-RT-0.4.0/Duke-RT/release-overlay"

# Duke 3D data. Override with DUKE3D_GRP=/path/to/DUKE3D.GRP if yours differs.
GRP="${DUKE3D_GRP:-}"
if [[ -z "$GRP" ]]; then
	for c in \
		"$HOME/.steam/debian-installation/steamapps/common/Duke Nukem 3D Twentieth Anniversary World Tour/DUKE3D.GRP" \
		"$HOME/.steam/steam/steamapps/common/Duke Nukem 3D Twentieth Anniversary World Tour/DUKE3D.GRP"
	do
		[[ -f "$c" ]] && { GRP="$c"; break; }
	done
fi

[[ -x "$RAZE"    ]] || { echo "raze not built: $RAZE"; exit 1; }
[[ -n "$GRP"     ]] || { echo "DUKE3D.GRP not found - set DUKE3D_GRP=/path/to/DUKE3D.GRP"; exit 1; }
[[ -d "$OVERLAY" ]] || { echo "overlay not found: $OVERLAY"; exit 1; }

# raze.pk3, libNRI.so and the compiled shaders all live beside the binary.
cd "$ROOT/build"
exec ./raze -gamegrp "$GRP" -file "$OVERLAY" "$@"
