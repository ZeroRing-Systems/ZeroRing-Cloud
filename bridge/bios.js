const terminal = document.getElementById("terminal");

const ws = new WebSocket("ws://localhost:8080");
ws.onmessage = (event) => {
  terminal.innerHTML += `[Cloud] ${event.data}<br>`;
};

const hal_implementation = {
  env: {
    print_text: (textPtr) => {
      terminal.innerHTML += `[Kernel] Text command received.<br>`;
    },
    network_request: (payloadPtr) => {
      terminal.innerHTML += `[BIOS] Intercepted Syscall. Routing to Cloud...<br>`;
      ws.send("Syscall intercepted from WASM kernel");
    },
  },
};

WebAssembly.instantiateStreaming(
  fetch("../wasm/kernel.wasm"),
  hal_implementation,
).then((obj) => {
  terminal.innerHTML += "[BIOS] Kernel loaded successfully.<br>";
});
