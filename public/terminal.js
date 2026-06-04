const terminal = document.getElementById("terminal");
const output = document.getElementById("output");
const inputLine = document.getElementById("input-line");
const promptEl = document.getElementById("prompt");
const cmdInput = document.getElementById("cmd");
const canvas = document.getElementById("framebuffer");
const ctx = canvas.getContext("2d");

let mem;
let wasmInstance;
let currentPrompt = "$ ";
let lastKey = 0;
let storageMode = "cloud";

const readStr = (ptr) => {
  const bytes = new Uint8Array(mem.buffer);
  const b = [];
  while (bytes[ptr]) b.push(bytes[ptr++]);
  return new TextDecoder().decode(new Uint8Array(b));
};

const writeStr = (str) => {
  const encoded = new TextEncoder().encode(str + "\0");
  const ptr = wasmInstance.exports.malloc(encoded.length);
  const view = new Uint8Array(mem.buffer);
  view.set(encoded, ptr);
  return { ptr, len: encoded.length - 1 };
};

const print = (text) => {
  if (text === "\x1b[clear]") {
    output.innerHTML = "";
    return;
  }
  const div = document.createElement("div");
  div.textContent = text;
  output.appendChild(div);
  terminal.scrollTop = terminal.scrollHeight;
};

let pendingMessages = [];
let wsReady = false;

const ws = new WebSocket("ws://localhost:8080");

ws.onopen = () => {
  wsReady = true;
  pendingMessages.forEach((msg) => ws.send(msg));
  pendingMessages = [];
};

ws.onmessage = (e) => {
  try {
    const data = JSON.parse(e.data);
    if (data.status === "ok") {
      if (data.fd !== undefined) {
        print("fd " + data.fd);
      } else if (data.bytes !== undefined) {
        print("wrote " + data.bytes + " bytes");
      } else if (data.files) {
        print(data.files.join("  "));
      } else if (data.data !== undefined) {
        print(data.data);
      } else {
        print("ok");
      }
    } else {
      print(data.message || "error");
    }
  } catch {
    print(e.data);
  }
};

ws.onerror = () => {
  if (!wsReady) print("backend offline");
};

ws.onclose = () => {
  wsReady = false;
};

const sendToCloud = (payload) => {
  if (wsReady && ws.readyState === 1) {
    ws.send(payload);
  } else {
    pendingMessages.push(payload);
  }
};

let opfsRoot = null;
const opfsFds = {};
let opfsNextFd = 100;

const initOPFS = async () => {
  if (opfsRoot) return opfsRoot;
  try {
    opfsRoot = await navigator.storage.getDirectory();
  } catch {
    print("opfs not available in this browser");
    opfsRoot = null;
  }
  return opfsRoot;
};

const opfsAction = async (payload) => {
  const msg = JSON.parse(payload);
  const root = await initOPFS();
  if (!root) {
    print("opfs not available");
    return;
  }

  if (msg.action === "list_files") {
    const names = [];
    for await (const key of root.keys()) names.push(key);
    print(names.join("  ") || "(empty)");
    return;
  }

  if (msg.action === "read_file") {
    try {
      const handle = await root.getFileHandle(msg.file);
      const file = await handle.getFile();
      const text = await file.text();
      print(text || "(empty)");
    } catch {
      print("not found");
    }
    return;
  }

  if (msg.action === "write_file") {
    const handle = await root.getFileHandle(msg.file, { create: true });
    const writable = await handle.createWritable();
    await writable.write(msg.data || "");
    await writable.close();
    print("ok");
    return;
  }

  if (msg.action === "delete_file") {
    try {
      await root.removeEntry(msg.file);
      print("ok");
    } catch {
      print("not found");
    }
    return;
  }

  if (msg.action === "open") {
    try {
      const create = (msg.flags || 0) === 1;
      const handle = await root.getFileHandle(msg.file, { create });
      const fd = opfsNextFd++;
      opfsFds[fd] = { handle, path: msg.file };
      print("fd " + fd);
    } catch {
      print("cannot open");
    }
    return;
  }

  if (msg.action === "fd_read") {
    const entry = opfsFds[msg.fd];
    if (!entry) { print("bad fd"); return; }
    const file = await entry.handle.getFile();
    const text = await file.text();
    print(text || "(empty)");
    return;
  }

  if (msg.action === "fd_write") {
    const entry = opfsFds[msg.fd];
    if (!entry) { print("bad fd"); return; }
    const writable = await entry.handle.createWritable();
    await writable.write(msg.data || "");
    await writable.close();
    print("wrote " + (msg.data || "").length + " bytes");
    return;
  }

  if (msg.action === "close") {
    if (!opfsFds[msg.fd]) { print("bad fd"); return; }
    delete opfsFds[msg.fd];
    print("ok");
    return;
  }

  print("unknown action");
};

const routeRequest = (payload) => {
  if (storageMode === "opfs") {
    opfsAction(payload).catch((e) => print("opfs error: " + e));
  } else {
    sendToCloud(payload);
  }
};

const fdTable = {};
let localFdCounter = 3;

const imports = {
  env: {
    js_print_text: (p) => print(readStr(p)),
    js_network_request: (p) => {
      routeRequest(readStr(p));
    },
    js_set_prompt: (p) => {
      currentPrompt = readStr(p);
      promptEl.textContent = currentPrompt;
    },
    js_read_key: () => {
      const k = lastKey;
      lastKey = 0;
      return k;
    },
    js_draw_pixel: (x, y, color) => {
      if (canvas.style.display === "none") canvas.style.display = "block";
      const r = (color >> 16) & 0xff;
      const g = (color >> 8) & 0xff;
      const b = color & 0xff;
      ctx.fillStyle = `rgb(${r},${g},${b})`;
      ctx.fillRect(x, y, 1, 1);
    },
    js_fs_open: (pathPtr, flags) => {
      const path = readStr(pathPtr);
      const fd = localFdCounter++;
      fdTable[fd] = { path, flags, data: "" };
      const payload = JSON.stringify({ action: "open", file: path, flags });
      routeRequest(payload);
      return fd;
    },
    js_fs_read: (fd, bufPtr, len) => {
      const payload = JSON.stringify({ action: "fd_read", fd });
      routeRequest(payload);
      return 0;
    },
    js_fs_write: (fd, dataPtr, len) => {
      const data = readStr(dataPtr);
      const payload = JSON.stringify({ action: "fd_write", fd, data });
      routeRequest(payload);
      return len;
    },
    js_fs_close: (fd) => {
      const payload = JSON.stringify({ action: "close", fd });
      routeRequest(payload);
      delete fdTable[fd];
      return 0;
    },
    js_set_storage: (p) => {
      const mode = readStr(p);
      if (mode === "cloud" || mode === "opfs") {
        storageMode = mode;
      }
    },
  },
};

const feedKey = (code) => {
  lastKey = code;
  if (wasmInstance && wasmInstance.exports.handle_key) {
    wasmInstance.exports.handle_key(code);
  }
};

document.addEventListener("keydown", (e) => {
  if (!wasmInstance) return;

  if (e.key === "Enter") {
    const echo = currentPrompt + cmdInput.textContent;
    print(echo);
    feedKey(13);
    cmdInput.textContent = "";
    e.preventDefault();
    return;
  }

  if (e.key === "Backspace") {
    feedKey(8);
    const t = cmdInput.textContent;
    cmdInput.textContent = t.slice(0, -1);
    e.preventDefault();
    return;
  }

  if (e.key.length === 1 && !e.ctrlKey && !e.metaKey) {
    feedKey(e.key.charCodeAt(0));
    cmdInput.textContent += e.key;
    e.preventDefault();
  }
});

terminal.addEventListener("click", () => terminal.focus());

WebAssembly.instantiateStreaming(fetch("wasm/kernel.wasm"), imports)
  .then(({ instance }) => {
    mem = instance.exports.memory;
    wasmInstance = instance;
    instance.exports.kernel_main();
    inputLine.style.display = "flex";
  })
  .catch((e) => print("boot failed: " + e));
