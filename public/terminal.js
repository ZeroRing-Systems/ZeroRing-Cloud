const terminal = document.getElementById("terminal");
const log = (msg) => {
  terminal.innerHTML += msg + "<br>";
};

let mem;
const readStr = (ptr) => {
  const bytes = new Uint8Array(mem.buffer);
  const b = [];
  while (bytes[ptr]) b.push(bytes[ptr++]);
  return new TextDecoder().decode(new Uint8Array(b));
};

let pendingMessages = [];
let wsReady = false;

const ws = new WebSocket("ws://localhost:8080");

ws.onopen = () => {
  wsReady = true;
  log("[Cloud] Connected.");
  pendingMessages.forEach((msg) => ws.send(msg));
  pendingMessages = [];
};

ws.onmessage = (e) => log("[Cloud] " + e.data);

ws.onerror = () => {
  if (!wsReady) log("[Cloud] Offline — backend not running.");
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

const imports = {
  env: {
    js_print_text: (p) => log("[Kernel] " + readStr(p)),
    js_network_request: (p) => {
      const payload = readStr(p);
      log("[BIOS] Syscall → " + payload);
      sendToCloud(payload);
    },
  },
};

log("[BIOS] Loading kernel...");
WebAssembly.instantiateStreaming(fetch("wasm/kernel.wasm"), imports)
  .then(({ instance }) => {
    mem = instance.exports.memory;
    log("[BIOS] kernel.wasm loaded.");
    instance.exports.kernel_main();
    log("[BIOS] Boot complete.");
  })
  .catch((e) => log("[BIOS] Error: " + e));
