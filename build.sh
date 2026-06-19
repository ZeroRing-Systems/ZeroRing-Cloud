#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT_DIR/build"
WASM_DIR="$ROOT_DIR/public/wasm"
WASM_FILE="$WASM_DIR/kernel.wasm"

mkdir -p "$BUILD_DIR" "$WASM_DIR"
cd "$BUILD_DIR"

cmake ..
cmake --build . -- -j"$(nproc)"

SERVER_BIN="$BUILD_DIR/server"
[[ -x "$SERVER_BIN" ]] || { echo "Error: backend server binary not found at $SERVER_BIN" >&2; exit 1; }
[[ -f "$WASM_FILE" ]] || { echo "Error: WebAssembly kernel missing at $WASM_FILE" >&2; exit 1; }

trap 'kill "$SERVER_PID" 2>/dev/null || true' EXIT INT TERM
"$SERVER_BIN" &
SERVER_PID=$!

cd "$ROOT_DIR"
python3 -m http.server 8000 --directory public
