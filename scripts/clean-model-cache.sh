#!/usr/bin/env bash

set -euo pipefail

# --------------------------------------------------
# Model cache cleaner (macOS / Linux)
#
# Lists the GGUF models that llama-server downloaded with -hf, with their
# sizes, and deletes the ones you pick. Deleted models are downloaded
# again the next time the server starts with them.
#
# Usage:
#   clean-model-cache.sh          pick models to delete
#   clean-model-cache.sh --list   only list them
#
# Where models are searched:
#   - Hugging Face cache (new llama.cpp):
#       $HF_HUB_CACHE, $HF_HOME/hub, ~/.cache/huggingface/hub
#   - llama.cpp cache (old llama.cpp):
#       $LLAMA_CACHE, ~/Library/Caches/llama.cpp (macOS), ~/.cache/llama.cpp (Linux)
#
# The Hugging Face cache is shared with other tools, so only folders that
# contain .gguf files are listed. The old llama.cpp cache keeps all models
# in one folder, so it is listed (and deleted) as a whole.
#
# Works with the bash bundled with macOS (3.2).
# --------------------------------------------------

LIST_ONLY=0

for arg in "$@"; do
    case "$arg" in
        --list)
            LIST_ONLY=1
            ;;
        *)
            echo "Usage: $0 [--list]"
            exit 1
            ;;
    esac
done

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}"

# Candidate folders (duplicates are skipped below)
CANDIDATES=()
[[ -n "${HF_HUB_CACHE:-}" ]] && CANDIDATES+=("$HF_HUB_CACHE")
[[ -n "${HF_HOME:-}" ]] && CANDIDATES+=("$HF_HOME/hub")
CANDIDATES+=("$CACHE_HOME/huggingface/hub")
[[ -n "${LLAMA_CACHE:-}" ]] && CANDIDATES+=("$LLAMA_CACHE")
CANDIDATES+=("$HOME/Library/Caches/llama.cpp" "$CACHE_HOME/llama.cpp")

# Models used by the launcher scripts (marked in the list)
USED_MODELS="$(grep -ho '"[^"]*-GGUF[^"]*"' "$SCRIPT_DIR"/llama-server-*.sh 2>/dev/null \
    | tr -d '"' | sed 's/:.*//' | sort -u || true)"

PATHS=()
LABELS=()
SEEN=" "

for dir in "${CANDIDATES[@]}"; do
    [[ -d "$dir" ]] || continue

    real="$(cd "$dir" && pwd -P)"
    case "$SEEN" in
        *" $real "*) continue ;;
    esac
    SEEN="$SEEN$real "

    # Hugging Face layout: one models--<owner>--<repo> folder per repository
    for model in "$real"/models--*; do
        [[ -d "$model" ]] || continue

        if [[ -z "$(find "$model" -name '*.gguf' -print 2>/dev/null | head -n 1)" ]]; then
            continue
        fi

        repo="$(basename "$model" | sed 's/^models--//; s/--/\//')"
        mark=""
        if printf '%s\n' "$USED_MODELS" | grep -qxF "$repo"; then
            mark="  [launcher]"
        fi

        PATHS+=("$model")
        LABELS+=("$repo$mark")
    done

    # Old llama.cpp layout: .gguf files directly in the folder
    if [[ -n "$(find "$real" -maxdepth 1 -name '*.gguf' -print 2>/dev/null | head -n 1)" ]]; then
        PATHS+=("$real")
        LABELS+=("(old llama.cpp cache, all models)")
    fi
done

if [[ ${#PATHS[@]} -eq 0 ]]; then
    echo "No downloaded GGUF models found."
    exit 0
fi

echo "=================================="
echo " Model Cache Cleaner"
echo "=================================="
echo

for i in "${!PATHS[@]}"; do
    size="$(du -sh "${PATHS[$i]}" 2>/dev/null | cut -f1)"
    printf '  %2d) %6s  %s\n' "$((i + 1))" "$size" "${LABELS[$i]}"
    printf '               %s\n' "${PATHS[$i]}"
done

echo
echo "  [launcher] = offered by llama-server-*.sh (bge-m3 is the RAG embedding model)"
echo

if [[ "$LIST_ONLY" == 1 ]]; then
    exit 0
fi

read -rp "Models to delete (e.g. 1 3, a = all, q = quit): " answer

SELECTED=()

case "$answer" in
    q|Q|"")
        exit 0
        ;;
    a|A)
        for i in "${!PATHS[@]}"; do
            SELECTED+=("$i")
        done
        ;;
    *)
        for n in $(echo "$answer" | tr ',' ' '); do
            if ! [[ "$n" =~ ^[0-9]+$ ]] || (( n < 1 || n > ${#PATHS[@]} )); then
                echo "Invalid selection: $n"
                exit 1
            fi
            SELECTED+=("$((n - 1))")
        done
        ;;
esac

echo
echo "These will be deleted (they cannot be restored, only downloaded again):"
for i in "${SELECTED[@]}"; do
    echo "  ${LABELS[$i]}"
    echo "    ${PATHS[$i]}"
done
echo

if pgrep -x llama-server >/dev/null 2>&1; then
    echo "Warning: llama-server is running. Stop it before deleting a model it uses."
    echo
fi

read -rp "Delete? [y/N]: " confirm

case "$confirm" in
    y|Y)
        ;;
    *)
        echo "Cancelled."
        exit 0
        ;;
esac

for i in "${SELECTED[@]}"; do
    path="${PATHS[$i]}"

    # Only delete what this script listed
    if [[ -d "$path" ]]; then
        rm -rf -- "$path"
        echo "Deleted: ${LABELS[$i]}"
    fi
done
