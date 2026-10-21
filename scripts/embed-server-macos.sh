#!/usr/bin/env bash

set -euo pipefail

# --------------------------------------------------
# llama.cpp embedding server launcher for RAG (macOS / Metal)
# BGE-M3 (multilingual)
#
# llama-server is searched in this order:
#   1. LLAMA_SERVER environment variable
#   2. $LLAMA_DIR/build/bin/llama-server (built from source)
#   3. llama-server in PATH (brew install llama.cpp)
#
# LLAMA_DIR defaults to ~/Repos/llama.cpp
# Run this alongside llama-server-macos.sh (port 8080).
# --------------------------------------------------

LLAMA_DIR="${LLAMA_DIR:-$HOME/Repos/llama.cpp}"

if [[ -n "${LLAMA_SERVER:-}" ]]; then
    SERVER="$LLAMA_SERVER"
elif [[ -x "$LLAMA_DIR/build/bin/llama-server" ]]; then
    SERVER="$LLAMA_DIR/build/bin/llama-server"
else
    SERVER="$(command -v llama-server || true)"
fi

HOST="127.0.0.1"
PORT=8081
MODEL="ggml-org/bge-m3-Q8_0-GGUF"
# Each chunk must fit in one batch, so batch = context
CONTEXT=8192
GPU_LAYERS=99

if [[ -z "$SERVER" || ! -x "$SERVER" ]]; then
    echo "Error: llama-server not found."
    echo "Install it with 'brew install llama.cpp' or build it in $LLAMA_DIR."
    exit 1
fi

echo "----------------------------------"
echo " Embedding  : BGE-M3"
echo " Backend    : Metal"
echo " Server     : $SERVER"
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
