#!/usr/bin/env bash

set -euo pipefail

# --------------------------------------------------
# llama.cpp embedding server launcher for RAG (Linux / Vulkan)
# BGE-M3 (multilingual)
#
# Override paths with environment variables:
#   LLAMA_DIR    llama.cpp directory (default: ~/Repos/llama.cpp)
#   LLAMA_SERVER llama-server executable
#
# Run this alongside llama-server-linux.sh (port 8080).
# --------------------------------------------------

LLAMA_DIR="${LLAMA_DIR:-$HOME/Repos/llama.cpp}"
SERVER="${LLAMA_SERVER:-$LLAMA_DIR/build/bin/llama-server}"

HOST="127.0.0.1"
PORT=8081
MODEL="ggml-org/bge-m3-Q8_0-GGUF"
# Each chunk must fit in one batch, so batch = context
CONTEXT=8192
GPU_LAYERS=99

if [[ ! -x "$SERVER" ]]; then
    echo "Error: llama-server not found: $SERVER"
    exit 1
fi

echo "----------------------------------"
echo " Embedding  : BGE-M3"
echo " Backend    : Vulkan"
echo " Endpoint   : http://$HOST:$PORT"
echo " Alias      : embed"
echo "----------------------------------"
echo

if [[ -d "$LLAMA_DIR" ]]; then
    cd "$LLAMA_DIR"
fi

exec "$SERVER" \
    -hf "$MODEL" \
    --embeddings \
    -ngl "$GPU_LAYERS" \
    -c "$CONTEXT" \
    -b "$CONTEXT" \
    -ub "$CONTEXT" \
    --alias embed \
    --host "$HOST" \
    --port "$PORT"
