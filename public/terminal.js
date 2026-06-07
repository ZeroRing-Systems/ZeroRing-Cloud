const terminal = document.getElementById("terminal");
const output = document.getElementById("output");
const inputLine = document.getElementById("input-line");
const promptEl = document.getElementById("prompt");
const cmdInput = document.getElementById("cmd");

let mem;
let wasmInstance;
let currentPrompt = "$ ";

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

ws.onmessage = function (e) {
    print(e.data);
};

const imports = {
    env: {
        js_print_text: function (p) {
            print(readStr(p));
        },
        js_set_prompt: function (p) {
            currentPrompt = readStr(p);
            promptEl.textContent = currentPrompt;
        },
        js_network_request: function (p) {
            if (ws.readyState === 1) ws.send(readStr(p));
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
