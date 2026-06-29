const terminal = document.getElementById("terminal");
const output   = document.getElementById("output");
const inputLine = document.getElementById("input-line");
const promptEl  = document.getElementById("prompt");
const cmdInput  = document.getElementById("cmd");

let mem;
let wasmInstance;
let currentPrompt = "$ ";

function readCStr(ptr) {
    const bytes = new Uint8Array(mem.buffer);
    let str = "";
    while (bytes[ptr]) {
        str += String.fromCharCode(bytes[ptr]);
        ptr++;
    }
    return str;
}

const SCRATCH_OFFSET = 64 * 1024;

function writeCStr(jsString) {
    const bytes = new Uint8Array(mem.buffer);
    for (let i = 0; i < jsString.length && i < 4095; i++) {
        bytes[SCRATCH_OFFSET + i] = jsString.charCodeAt(i);
    }
    bytes[SCRATCH_OFFSET + Math.min(jsString.length, 4095)] = 0;
    return SCRATCH_OFFSET;
}

function printLine(text) {
    if (text === "\x1b[clear]") {
        output.innerHTML = "";
        return;
    }
    const div = document.createElement("div");
    div.textContent = text;
    output.appendChild(div);
    terminal.scrollTop = terminal.scrollHeight;
}

let ws = null;
let wsReconnectTimer = null;

function connectWebSocket() {
    const wsUrl = `ws://${location.hostname || "localhost"}:8080`;
    ws = new WebSocket(wsUrl);

    ws.onopen = function () {
        console.log("[ZeroRing] WebSocket connected to", wsUrl);
    };

    ws.onmessage = function (e) {
        if (wasmInstance && wasmInstance.exports.handle_net_response) {
            const ptr = writeCStr(e.data);
            wasmInstance.exports.handle_net_response(ptr);
        } else {
            printLine(e.data);
        }
    };

    ws.onclose = function () {
        console.warn("[ZeroRing] WebSocket disconnected. Reconnecting in 3s...");
        wsReconnectTimer = setTimeout(connectWebSocket, 3000);
    };

    ws.onerror = function (err) {
        console.error("[ZeroRing] WebSocket error:", err);
        ws.close();
    };
}

connectWebSocket();

const imports = {
    env: {
        js_print: function (ptr) {
            printLine(readCStr(ptr));
        },

        js_set_prompt: function (ptr) {
            currentPrompt = readCStr(ptr);
            promptEl.textContent = currentPrompt;
        },

        js_clear_screen: function () {
            output.innerHTML = "";
        },

        js_net_send: function (ptr) {
            const json = readCStr(ptr);
            if (ws && ws.readyState === WebSocket.OPEN) {
                ws.send(json);
            } else {
                printLine("[net] error: not connected to backend");
            }
        },

        js_log_error: function (ptr) {
            console.error("[ZeroKernel]", readCStr(ptr));
        },
    },
};

function feedKey(code) {
    if (wasmInstance && wasmInstance.exports.handle_key) {
        wasmInstance.exports.handle_key(code);
    }
}

document.addEventListener("keydown", function (e) {
    if (!wasmInstance) return;

    if (e.key === "Enter") {
        printLine(currentPrompt + cmdInput.textContent);
        feedKey(13);
        cmdInput.textContent = "";
        e.preventDefault();
        return;
    }

    if (e.key === "Backspace") {
        feedKey(8);
        cmdInput.textContent = cmdInput.textContent.slice(0, -1);
        e.preventDefault();
        return;
    }

    if (e.key.length === 1 && !e.ctrlKey && !e.metaKey) {
        feedKey(e.key.charCodeAt(0));
        cmdInput.textContent += e.key;
        e.preventDefault();
    }
});

terminal.addEventListener("click", function () {
    terminal.focus();
});

WebAssembly.instantiateStreaming(fetch("wasm/kernel.wasm?v=" + Date.now()), imports)
    .then(function (result) {
        mem = result.instance.exports.memory;
        wasmInstance = result.instance;
        result.instance.exports.kernel_main();
        inputLine.style.display = "flex";
    })
    .catch(function (e) {
        printLine("boot failed: " + e);
        console.error(e);
    });
