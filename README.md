# ☁️ ZeroRing-Cloud

**ZeroRing-Cloud** acts as the "Motherboard" and "Hard Drive" for the ZeroRing-Systems architecture. It fulfills the hardware contracts established by `ZeroKernel`.

### 🏗️ Monorepo Structure
This repository contains two strictly isolated modules:

#### 1. `/bridge` (The Motherboard & Display)
The frontend UI and the JavaScript interop layer. 
* **`bios.js`**: The magic layer. It intercepts virtual calls from the WASM kernel and routes them to browser APIs (like drawing to the HTML5 Canvas) or proxies them over WebSockets.
* **Execution:** Drop the compiled `kernel.wasm` from the `ZeroKernel` repository into this folder to boot the OS.

#### 2. `/backend` (The Hard Drive & Network)
The persistent C++ backend server.
* **WebSocket Server:** Built with Boost, it maintains a persistent connection with the browser to receive and process intercepted system calls (like `open()` or `write()`).
* **Virtual File System:** Backed by PostgreSQL, this isolates and manages user directories, executable blobs, and data states, acting as the physical `C:` drive for the browser OS.

### 🚀 Running Locally
*(Instructions for launching the C++ Backend and serving the Frontend will be added here as the build pipelines are finalized.)*
