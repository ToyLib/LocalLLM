#!/usr/bin/env bash

set -euo pipefail

# --------------------------------------------------
# llama.cpp server launcher (Linux / Vulkan)
# Qwen3 / Gemma3 / Phi-4 mini
#
# Also starts the embedding server for RAG (BGE-M3, port 8081) in the
# background and stops it when the chat server exits.
# Its log goes to logs/embed-server.log.
#
# Usage:
#   llama-server-linux.sh             chat + embedding server
#   llama-server-linux.sh --no-embed  chat server only (no RAG)
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

# Embedding server for RAG
# Each chunk must fit in one batch, so batch = context
EMBED_PORT=8081
EMBED_MODEL="ggml-org/bge-m3-Q8_0-GGUF"
EMBED_CONTEXT=8192

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$(dirname "$SCRIPT_DIR")/logs"
LOG_FILE="$LOG_DIR/embed-server.log"

START_EMBED=1

for arg in "$@"; do
    case "$arg" in
        --no-embed)
            START_EMBED=0
            ;;
        *)
            echo "Usage: $0 [--no-embed]"
            exit 1
            ;;
    esac
done

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
if [[ "$START_EMBED" == 1 ]]; then
    echo " Embedding  : BGE-M3 (http://$HOST:$EMBED_PORT, alias embed)"
else
    echo " Embedding  : off"
fi
echo "----------------------------------"
echo

if [[ "$START_EMBED" == 1 ]]; then
    mkdir -p "$LOG_DIR"

    "$SERVER" \
        -hf "$EMBED_MODEL" \
        --embeddings \
        -ngl "$GPU_LAYERS" \
        -c "$EMBED_CONTEXT" \
        -b "$EMBED_CONTEXT" \
        -ub "$EMBED_CONTEXT" \
        --alias embed \
        --host "$HOST" \
        --port "$EMBED_PORT" \
        > "$LOG_FILE" 2>&1 < /dev/null &
    EMBED_PID=$!

    # Stop the embedding server when this script exits (Ctrl+C included)
    cleanup() {
        if kill -0 "$EMBED_PID" 2>/dev/null; then
            echo "Stopping embedding server (PID $EMBED_PID)..."
            kill "$EMBED_PID" 2>/dev/null || true
            wait "$EMBED_PID" 2>/dev/null || true
        fi
    }
    trap cleanup EXIT

    # Fail early if it could not start (e.g. port in use)
    sleep 1
    if ! kill -0 "$EMBED_PID" 2>/dev/null; then
        echo "Error: embedding server exited. Log: $LOG_FILE"
        tail -n 20 "$LOG_FILE"
        exit 1
    fi

    echo "Embedding server is running in the background (PID $EMBED_PID)"
    echo "  Log: $LOG_FILE"
    echo
fi

if [[ -d "$LLAMA_DIR" ]]; then
    cd "$LLAMA_DIR"
fi

# Not exec, so that the trap above can stop the embedding server
"$SERVER" \
    -hf "$MODEL" \
    -ngl "$GPU_LAYERS" \
    -c "$CONTEXT" \
    --alias local \
    --host "$HOST" \
    --port "$PORT"
