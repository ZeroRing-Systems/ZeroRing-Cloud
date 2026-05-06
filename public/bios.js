const terminal = document.getElementById("terminal");
const log = (msg) => {
  terminal.innerHTML += msg + "<br>";
};
let mem;
const readStr = (ptr) => {
  const b = [];
  while (mem[ptr]) b.push(mem[ptr++]);
  return new TextDecoder().decode(new Uint8Array(b));
};
const ws = new WebSocket("ws://localhost:8080");
ws.onopen = () => log("[Cloud] Connected.");
ws.onmessage = (e) => log("[Cloud] " + e.data);
ws.onerror = () => log("[Cloud] Offline.");
const imports = {
  env: {
    js_print_text: (p) => log("[Kernel] " + readStr(p)),
    js_network_request: (p) => {
      log("[BIOS] Syscall → " + readStr(p));
      ws.readyState === 1 && ws.send(readStr(p));
    },
  },
};
log("[BIOS] Loading kernel...");
WebAssembly.instantiateStreaming(fetch("wasm/kernel.wasm"), imports)
  .then(({ instance }) => {
    mem = new Uint8Array(instance.exports.memory.buffer);
    log("[BIOS] kernel.wasm loaded.");
    instance.exports.kernel_main();
    log("[BIOS] Boot complete.");
  })
  .catch((e) => log("[BIOS] Error: " + e));
