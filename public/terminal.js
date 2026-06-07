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

function readStr(ptr) {
    const bytes = new Uint8Array(mem.buffer);
    let str = "";
    while (bytes[ptr]) {
        str += String.fromCharCode(bytes[ptr]);
        ptr++;
    }
    return str;
}

function print(text) {
    if (text === "\x1b[clear]") {
        output.innerHTML = "";
        return;
    }
    const div = document.createElement("div");
    div.textContent = text;
    output.appendChild(div);
    terminal.scrollTop = terminal.scrollHeight;
}

let ws = new WebSocket("ws://localhost:8080");
let wsReady = false;
let pendingMessages = [];

ws.onopen = function () {
    wsReady = true;
    for (let i = 0; i < pendingMessages.length; i++) {
        ws.send(pendingMessages[i]);
    }
    pendingMessages = [];
};

ws.onmessage = function (e) {
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
    } catch (err) {
        print(e.data);
    }
};

ws.onerror = function () {
    if (!wsReady) print("backend offline");
};

ws.onclose = function () {
    wsReady = false;
};

function sendToBackend(payload) {
    if (wsReady && ws.readyState === 1) {
        ws.send(payload);
    } else {
        pendingMessages.push(payload);
    }
}

let fdTable = {};
let localFdCounter = 3;

const imports = {
    env: {
        js_print_text: function (p) {
            print(readStr(p));
        },
        js_network_request: function (p) {
            sendToBackend(readStr(p));
        },
        js_set_prompt: function (p) {
            currentPrompt = readStr(p);
            promptEl.textContent = currentPrompt;
        },
        js_read_key: function () {
            const k = lastKey;
            lastKey = 0;
            return k;
        },
        js_draw_pixel: function (x, y, color) {
            if (canvas.style.display === "none") canvas.style.display = "block";
            const r = (color >> 16) & 0xff;
            const g = (color >> 8) & 0xff;
            const b = color & 0xff;
            ctx.fillStyle = "rgb(" + r + "," + g + "," + b + ")";
            ctx.fillRect(x, y, 1, 1);
        },
        js_fs_open: function (pathPtr, flags) {
            const path = readStr(pathPtr);
            const fd = localFdCounter++;
            fdTable[fd] = { path: path, flags: flags };
            const payload = JSON.stringify({ action: "open", file: path, flags: flags });
            sendToBackend(payload);
            return fd;
        },
        js_fs_read: function (fd, bufPtr, len) {
            const payload = JSON.stringify({ action: "fd_read", fd: fd });
            sendToBackend(payload);
            return 0;
        },
        js_fs_write: function (fd, dataPtr, len) {
            const data = readStr(dataPtr);
            const payload = JSON.stringify({ action: "fd_write", fd: fd, data: data });
            sendToBackend(payload);
            return len;
        },
        js_fs_close: function (fd) {
            const payload = JSON.stringify({ action: "close", fd: fd });
            sendToBackend(payload);
            delete fdTable[fd];
            return 0;
        },
    },
};

function feedKey(code) {
    lastKey = code;
    if (wasmInstance && wasmInstance.exports.handle_key) {
        wasmInstance.exports.handle_key(code);
    }
}

document.addEventListener("keydown", function (e) {
    if (!wasmInstance) return;

    if (e.key === "Enter") {
        print(currentPrompt + cmdInput.textContent);
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

WebAssembly.instantiateStreaming(fetch("wasm/kernel.wasm"), imports)
    .then(function (result) {
        mem = result.instance.exports.memory;
        wasmInstance = result.instance;
        result.instance.exports.kernel_main();
        inputLine.style.display = "flex";
    })
    .catch(function (e) {
        print("boot failed: " + e);
    });
