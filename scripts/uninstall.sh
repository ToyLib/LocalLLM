#!/usr/bin/env bash

set -euo pipefail

# --------------------------------------------------
# Uninstaller (macOS / Linux)
#
# Removes what LocalLLM leaves outside its own folder. Two groups are
# listed with their sizes, and each one is confirmed separately:
#
#   1. Settings and data of LocalLLM (the whole folder, including the
#      API key, the logs and the downloaded llama-server):
#        macOS: ~/Library/Application Support/LocalLLM
#        Linux: $XDG_CONFIG_HOME/LocalLLM or ~/.config/LocalLLM
#   2. Models that llama-server downloaded:
#        Hugging Face cache (new llama.cpp):
#          $HF_HUB_CACHE, $HF_HOME/hub, ~/.cache/huggingface/hub
#        llama.cpp cache (old llama.cpp):
#          ~/Library/Caches/llama.cpp (macOS), ~/.cache/llama.cpp (Linux),
#          $LLAMA_CACHE
#
# The Hugging Face cache is shared with other tools, so only the model
# folders that contain .gguf files are removed. The old llama.cpp cache
# belongs to llama.cpp, so it is removed as a whole.
#
# The application itself (the repository or the extracted folder) and a
# folder set with LOCALLLM_CONFIG_DIR are not touched.
#
# Usage:
#   uninstall.sh          confirm and delete
#   uninstall.sh --list   only list what would be deleted
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

if [[ -z "${HOME:-}" ]]; then
    echo "HOME is not set."
    exit 1
fi

# Same place as UserDataDirectory (src/common/config.cpp) and user_data_directory (python/common/config.py)
if [[ "$(uname -s)" == "Darwin" ]]; then
    DATA_DIR="$HOME/Library/Application Support/LocalLLM"
else
    DATA_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/LocalLLM"
fi

CACHE_HOME="${XDG_CACHE_HOME:-$HOME/.cache}"

# Parallel arrays: what to delete, how to show it, its Hugging Face lock folder
MODEL_PATHS=()
MODEL_LABELS=()
MODEL_LOCKS=()
SEEN=" "

# A cache folder set by the environment must not take the home folder with it
is_protected() {
    local dir="$1"
    local home

    home="$(cd "$HOME" 2>/dev/null && pwd -P)" || home="$HOME"

    if [[ "$dir" == "/" ]]; then
        return 0
    fi

    case "$home/" in
        "$dir"/*) return 0 ;;
    esac

    return 1
}

# Hugging Face layout: one models--<owner>--<repo> folder per repository
add_model_folders() {
    local root="$1"
    local model name repo

    for model in "$root"/models--*; do
        [[ -d "$model" ]] || continue

        if [[ -z "$(find "$model" -name '*.gguf' -print 2>/dev/null | head -n 1)" ]]; then
            continue
        fi

        name="$(basename "$model")"
        repo="$(printf '%s\n' "$name" | sed 's/^models--//; s/--/\//')"

        MODEL_PATHS+=("$model")
        MODEL_LABELS+=("$repo")
        MODEL_LOCKS+=("$root/.locks/$name")
    done
}

# whole: "never"  only the GGUF model folders inside (shared with other tools)
#        "always" the folder itself (it belongs to llama.cpp)
#        "gguf"   the folder itself if it has .gguf files directly in it
add_cache_folder() {
    local dir="$1"
    local whole="$2"
    local real

    [[ -d "$dir" ]] || return 0

    real="$(cd "$dir" && pwd -P)"
    case "$SEEN" in
        *" $real "*) return 0 ;;
    esac
    SEEN="$SEEN$real "

    if [[ "$whole" == "gguf" ]]; then
        if [[ -n "$(find "$real" -maxdepth 1 -name '*.gguf' -print 2>/dev/null | head -n 1)" ]]; then
            whole="always"
        else
            whole="never"
        fi
    fi

    if [[ "$whole" == "always" ]]; then
        if is_protected "$real"; then
            echo "Skipped (contains your home folder): $real"
        else
            MODEL_PATHS+=("$real")
            MODEL_LABELS+=("(old llama.cpp cache, all models)")
            MODEL_LOCKS+=("")
            return 0
        fi
    fi

    add_model_folders "$real"
}

# The Hugging Face folders come first: a folder listed twice must be treated as shared
[[ -z "${HF_HUB_CACHE:-}" ]] || add_cache_folder "$HF_HUB_CACHE" never
[[ -z "${HF_HOME:-}" ]] || add_cache_folder "$HF_HOME/hub" never
add_cache_folder "$CACHE_HOME/huggingface/hub" never
add_cache_folder "$HOME/Library/Caches/llama.cpp" always
add_cache_folder "$CACHE_HOME/llama.cpp" always
[[ -z "${LLAMA_CACHE:-}" ]] || add_cache_folder "$LLAMA_CACHE" gguf

HAS_DATA=0
[[ ! -e "$DATA_DIR" ]] || HAS_DATA=1

if [[ "$HAS_DATA" == 0 && ${#MODEL_PATHS[@]} -eq 0 ]]; then
    echo "Nothing to remove."
    exit 0
fi

folder_size() {
    du -sh "$1" 2>/dev/null | cut -f1 || true
}

echo "=================================="
echo " LocalLLM Uninstaller"
echo "=================================="
echo

if [[ "$HAS_DATA" == 1 ]]; then
    echo "[1] Settings and data (settings, API key, logs, downloaded llama-server)"
    printf '  %6s  %s\n' "$(folder_size "$DATA_DIR")" "$DATA_DIR"
else
    echo "[1] Settings and data: none"
fi
echo

if [[ ${#MODEL_PATHS[@]} -gt 0 ]]; then
    echo "[2] Downloaded models (bge-m3 is the RAG embedding model)"
    for i in "${!MODEL_PATHS[@]}"; do
        printf '  %6s  %s\n' "$(folder_size "${MODEL_PATHS[$i]}")" "${MODEL_LABELS[$i]}"
        printf '          %s\n' "${MODEL_PATHS[$i]}"
    done
else
    echo "[2] Downloaded models: none"
fi
echo

if [[ -n "${LOCALLLM_CONFIG_DIR:-}" ]]; then
    echo "Not touched: LOCALLLM_CONFIG_DIR ($LOCALLLM_CONFIG_DIR)"
    echo
fi

if [[ "$LIST_ONLY" == 1 ]]; then
    exit 0
fi

if pgrep -x llama-server >/dev/null 2>&1 || pgrep -x LocalLLM >/dev/null 2>&1; then
    echo "Warning: LocalLLM or llama-server is running. Quit it before deleting."
    echo
fi

# Everything is asked first, so that nothing is deleted before the last answer
ask() {
    local answer=""

    read -rp "$1 [y/N]: " answer || true
    [[ "$answer" == "y" || "$answer" == "Y" ]]
}

DELETE_DATA=0
DELETE_MODELS=0

if [[ "$HAS_DATA" == 1 ]] && ask "Delete [1] settings and data? The API key is deleted too and cannot be restored."; then
    DELETE_DATA=1
fi

if [[ ${#MODEL_PATHS[@]} -gt 0 ]] && ask "Delete [2] all downloaded models listed above? Other tools that use them will need them downloaded again."; then
    DELETE_MODELS=1
fi

if [[ "$DELETE_DATA" == 0 && "$DELETE_MODELS" == 0 ]]; then
    echo "Nothing was deleted."
    exit 0
fi

echo

FAILED=0

delete_path() {
    if rm -rf -- "$1"; then
        echo "Deleted: $2"
    else
        echo "Failed: $2 ($1)"
        FAILED=1
    fi
}

if [[ "$DELETE_DATA" == 1 ]]; then
    delete_path "$DATA_DIR" "settings and data"
fi

if [[ "$DELETE_MODELS" == 1 ]]; then
    for i in "${!MODEL_PATHS[@]}"; do
        delete_path "${MODEL_PATHS[$i]}" "${MODEL_LABELS[$i]}"

        if [[ -n "${MODEL_LOCKS[$i]}" ]]; then
            rm -rf -- "${MODEL_LOCKS[$i]}" || true
        fi
    done
fi

echo
echo "The application folder itself was not removed. Delete it yourself if you no longer need it."

exit "$FAILED"
