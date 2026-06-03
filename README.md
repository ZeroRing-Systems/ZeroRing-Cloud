# ☁️ ZeroRing-Cloud

**ZeroRing-Cloud** acts as the "Motherboard" and "Hard Drive" for the ZeroRing-Systems architecture. It fulfills the hardware contracts established by `ZeroKernel`.

### 🏗️ Structure

#### `/public` (The Motherboard & Display)
The frontend UI and the JavaScript interop layer.
* **`terminal.js`**: Intercepts virtual calls from the WASM kernel and routes them to browser APIs or proxies them over WebSockets to the backend.
* **`index.html`**: The terminal display rendered in the browser.
* **`wasm/`**: Build artifact directory — receives `kernel.wasm` from the build pipeline.

#### `/backend` (The Hard Drive & Network)
The persistent C++ backend server.
* **`server.cpp`**: WebSocket server built on raw POSIX sockets. Accepts connections, performs the RFC 6455 handshake, and routes incoming syscalls to the virtual filesystem.
* **`websocket.h`**: Zero-dependency WebSocket protocol implementation (SHA-1, base64, frame encode/decode).
* **`db_manager.h`**: In-memory virtual filesystem with read, write, delete, list, and exists operations.

### 🚀 Running Locally
From the repository root:
```bash
./build.sh
```
This compiles the WASM kernel, builds the C++ backend, and launches both servers (backend on `:8080`, frontend on `:8000`).
