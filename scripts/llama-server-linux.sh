#!/usr/bin/env bash

set -euo pipefail

# --------------------------------------------------
# llama.cpp server launcher (Linux / Vulkan)
# Qwen3 / Gemma3 / Phi-4 mini
#
# Override paths with environment variables:
#   LLAMA_DIR    llama.cpp directory (default: ~/Repos/llama.cpp)
#   LLAMA_SERVER llama-server executable
# --------------------------------------------------

LLAMA_DIR="${LLAMA_DIR:-$HOME/Repos/llama.cpp}"
SERVER="${LLAMA_SERVER:-$LLAMA_DIR/build/bin/llama-server}"

HOST="127.0.0.1"
PORT=8080
CONTEXT=4096
GPU_LAYERS=99

if [[ ! -x "$SERVER" ]]; then
    echo "Error: llama-server not found: $SERVER"
    exit 1
fi

echo "=================================="
echo " Local LLM Server Launcher"
echo "=================================="
echo
echo "  1) Qwen3 4B"
echo "  2) Gemma 3 4B"
echo "  3) Phi-4 mini"
echo "  q) Quit"
echo

read -rp "Select model [1-3]: " choice

case "$choice" in
    1)
        NAME="Qwen3 4B"
        MODEL="Qwen/Qwen3-4B-GGUF:Q4_K_M"
        ;;
    2)
        NAME="Gemma 3 4B"
        MODEL="ggml-org/gemma-3-4b-it-GGUF"
        ;;
    3)
        NAME="Phi-4 mini"
        MODEL="bartowski/microsoft_Phi-4-mini-instruct-GGUF:Q4_K_M"
        ;;
    q|Q)
        exit 0
        ;;
    *)
        echo "Invalid selection."
        exit 1
        ;;
esac

echo
echo "----------------------------------"
echo " Model      : $NAME"
echo " Backend    : Vulkan"
echo " GPU layers : $GPU_LAYERS"
echo " Context    : $CONTEXT"
echo " Endpoint   : http://$HOST:$PORT"
echo " Alias      : local"
echo "----------------------------------"
echo

if [[ -d "$LLAMA_DIR" ]]; then
    cd "$LLAMA_DIR"
fi

exec "$SERVER" \
    -hf "$MODEL" \
    -ngl "$GPU_LAYERS" \
    -c "$CONTEXT" \
    --alias local \
    --host "$HOST" \
    --port "$PORT"
