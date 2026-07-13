# ZeroRing-Cloud

The frontend and backend for ZeroRing OS — a WebAssembly-powered terminal operating system that runs in the browser.

**🌐 Live:** [zeroring-cloud.vercel.app](https://zeroring-cloud.vercel.app)

## Architecture

```
┌─────────────────────┐     wss://      ┌──────────────────────────┐
│   Vercel (Frontend)  │ ──────────────► │   Azure VM (Backend)      │
│                      │                 │                            │
│  index.html          │                 │  Nginx (SSL termination)   │
│  terminal.html       │                 │    ↓                       │
│  terminal.js         │                 │  server (C++ WebSocket)    │
│  wasm/kernel.wasm    │                 │    ↓                       │
└─────────────────────┘                 │  PostgreSQL (persistent)   │
                                         └──────────────────────────┘
```

## What it does

- Serves a terminal UI in the browser that loads the WASM kernel
- Runs a concurrent WebSocket server that manages a persistent virtual filesystem
- The kernel sends commands (`ls`, `cat`, `write`, etc.) and the server handles them
- Session isolation: each user gets their own sandboxed filesystem
- Supports sandboxed Python script execution via the `run` command

## Project Structure

### Frontend (`public/`)
| File | Description |
|---|---|
| `index.html` | Landing page with project overview |
| `terminal.html` | Terminal UI with built-in code editor overlay |
| `terminal.js` | Loads WASM kernel, handles keyboard input, WebSocket communication |
| `wasm/kernel.wasm` | Compiled ZeroKernel (C++ → WebAssembly) |

### Backend (`backend/`)
| File | Description |
|---|---|
| `server.cpp` | Concurrent WebSocket server using POSIX sockets + threads |
| `websocket.h` | WebSocket handshake and frame encoding/decoding (OpenSSL SHA-1) |
| `json_util.h` | Lightweight JSON parser and builder |
| `db_manager.h` | Database manager interface (VFS abstraction) |
| `db_manager_pg.cpp` | PostgreSQL-backed persistent filesystem |
| `db_manager_mem.cpp` | In-memory filesystem fallback |

### CI/CD
| File | Description |
|---|---|
| `.github/workflows/deploy.yml` | GitHub Actions: auto-deploys backend on push |
| `deploy.sh` | Manual deploy script (SSH into VM, pull, build, restart) |

## Deployment

### Frontend (Vercel)
Automatically deployed on every push to `main` via Vercel. No build step required — static files served directly.

### Backend (Azure VM)
Automatically deployed via GitHub Actions when files in `backend/` or `CMakeLists.txt` change.

**Manual deploy:**
```bash
./deploy.sh
```

### Local Development

```bash
# Build backend
mkdir -p build && cd build
cmake .. -DUSE_POSTGRES=OFF   # Use in-memory VFS for local dev
make -j$(nproc)
./server                       # Starts on ws://localhost:8080

# Serve frontend
cd public
python3 -m http.server 8000   # Open http://localhost:8000
```

## Tech Stack

| Component | Technology |
|---|---|
| Frontend | Vanilla HTML/JS, WebAssembly |
| Backend | C++17, POSIX sockets, pthreads |
| Database | PostgreSQL 16 (via libpqxx) |
| SSL | Let's Encrypt + Nginx reverse proxy |
| Frontend Hosting | Vercel |
| Backend Hosting | Azure VM (Ubuntu 24.04 LTS) |
| CI/CD | GitHub Actions |
