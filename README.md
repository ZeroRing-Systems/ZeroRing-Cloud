# ZeroRing-Cloud

The frontend and backend for ZeroRing OS.

## What it does

- Shows a terminal in the browser that connects to the WASM kernel
- Runs a WebSocket server that acts as a virtual filesystem
- The kernel sends commands like "read file" or "write file" and the server handles them

## Files

### Frontend (`public/`)
- `index.html` — the terminal UI
- `terminal.js` — loads the WASM kernel, handles keyboard input, talks to the backend over WebSocket

### Backend (`backend/`)
- `server.cpp` — WebSocket server using POSIX sockets
- `websocket.h` — WebSocket handshake and frame encoding/decoding (uses OpenSSL for SHA-1)
- `db_manager.h` — in-memory filesystem (just a `std::map`)

## Run

From the repo root:
```bash
./build.sh
```

Backend runs on `localhost:8080`, frontend on `localhost:8000`.
