#!/usr/bin/env bash
#
# run.sh - PS3Recomp launcher with Render-Target Write-Color-Buffer (WCB)
# and HeadFaceCacheFile auto-purge support for MLB The Show 16.
#
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

PURGE_CACHE=0
VERBOSE=0
GAME_BIN=""
GAME_ARGS=()

for arg in "$@"; do
    case "$arg" in
        --purge-cache|--clean-face-cache|-p)
            PURGE_CACHE=1
            ;;
        --verbose|-v)
            VERBOSE=1
            ;;
        --help|-h)
            echo "Usage: $0 [options] [--] [binary] [game arguments...]"
            echo ""
            echo "Options:"
            echo "  --purge-cache, -p     Purge stale HeadFaceCacheFile to force regeneration with WCB"
            echo "  --verbose, -v         Enable verbose launch logging"
            echo "  --help, -h            Show this help message"
            echo ""
            echo "Environment variables:"
            echo "  PURGE_FACE_CACHE=1    Auto-purge HeadFaceCacheFile on startup"
            echo "  PS3RECOMP_RSX_WCB=1   Enable Vulkan/Draw-Engine Write-Color-Buffer (enabled by default)"
            exit 0
            ;;
        *)
            if [ -z "$GAME_BIN" ] && [ -x "$arg" ]; then
                GAME_BIN="$arg"
            else
                GAME_ARGS+=("$arg")
            fi
            ;;
    esac
done

if [ "${PURGE_FACE_CACHE:-0}" = "1" ]; then
    PURGE_CACHE=1
fi

# Enable WCB by default for accurate render target readback (fixes player faces & crowd textures)
export PS3RECOMP_RSX_WCB="${PS3RECOMP_RSX_WCB:-1}"

# Purge corrupted/stale face cache if requested
if [ "$PURGE_CACHE" = "1" ]; then
    echo "[run.sh] Scanning for stale HeadFaceCacheFile entries..."
    FOUND_CACHES=$(find . -maxdepth 5 -type f -name "*HeadFaceCache*" 2>/dev/null || true)
    if [ -n "$FOUND_CACHES" ]; then
        echo "$FOUND_CACHES" | while IFS= read -r f; do
            if [ -n "$f" ]; then
                echo "[run.sh] Purging stale face cache: $f"
                rm -f "$f"
            fi
        done
        echo "[run.sh] Face cache purged. MLB 16 will regenerate with active WCB."
    else
        echo "[run.sh] No existing HeadFaceCacheFile found (clean state)."
    fi
fi

# Locate binary if not explicitly provided
if [ -z "$GAME_BIN" ]; then
    for candidate in "${ROOT_DIR}/build/mlb16" "${ROOT_DIR}/build/game" "./mlb16" "./build/mlb16" "./game" "./EBOOT.BIN"; do
        if [ -x "$candidate" ]; then
            GAME_BIN="$candidate"
            break
        fi
    done
fi

if [ -n "$GAME_BIN" ]; then
    if [ "$VERBOSE" = "1" ]; then
        echo "[run.sh] Executing: $GAME_BIN ${GAME_ARGS[*]}"
        echo "[run.sh] PS3RECOMP_RSX_WCB=${PS3RECOMP_RSX_WCB}"
    fi
    exec "$GAME_BIN" "${GAME_ARGS[@]}"
else
    echo "[run.sh] Environment configured (PS3RECOMP_RSX_WCB=${PS3RECOMP_RSX_WCB})."
    if [ "$PURGE_CACHE" = "1" ]; then
        echo "[run.sh] Face cache purge completed successfully."
    fi
fi
