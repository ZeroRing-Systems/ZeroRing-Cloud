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

const readStr = (ptr) => {
  const bytes = new Uint8Array(mem.buffer);
  const b = [];
  while (bytes[ptr]) b.push(bytes[ptr++]);
  return new TextDecoder().decode(new Uint8Array(b));
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
      if (data.files) {
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

const imports = {
  env: {
    js_print_text: (p) => print(readStr(p)),
    js_network_request: (p) => {
      sendToCloud(readStr(p));
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
      const r = (color >> 16) & 0xff;
      const g = (color >> 8) & 0xff;
      const b = color & 0xff;
      ctx.fillStyle = `rgb(${r},${g},${b})`;
      ctx.fillRect(x, y, 1, 1);
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
