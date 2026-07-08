const terminal = document.getElementById("terminal");
const output   = document.getElementById("output");
const inputLine = document.getElementById("input-line");
const promptEl  = document.getElementById("prompt");
const cmdInput  = document.getElementById("cmd");

let mem;
let wasmInstance;
let currentPrompt = "$ ";

const commandHistory = [];
let historyIndex = -1;
let savedInput = "";
let pendingTabPrefix = null;
let currentCwd = "/";
let sessionId = localStorage.getItem("zeroring_session") || "";

function generateSessionId() {
    const arr = new Uint8Array(16);
    crypto.getRandomValues(arr);
    return Array.from(arr, function(b) { return b.toString(16).padStart(2, "0"); }).join("");
}

if (!sessionId) {
    sessionId = generateSessionId();
    localStorage.setItem("zeroring_session", sessionId);
}

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

function syncWasmLine(text) {
    if (!wasmInstance || !wasmInstance.exports.handle_key) return;
    wasmInstance.exports.handle_key(21);
    for (let i = 0; i < text.length; i++) {
        wasmInstance.exports.handle_key(text.charCodeAt(i));
    }
}

const ANSI_COLORS = {
    "30": "#1e1e1e", "31": "#e06c75", "32": "#98c379", "33": "#e5c07b",
    "34": "#61afef", "35": "#c678dd", "36": "#56b6c2", "37": "#abb2bf",
    "90": "#5c6370", "91": "#e06c75", "92": "#98c379", "93": "#e5c07b",
    "94": "#61afef", "95": "#c678dd", "96": "#56b6c2", "97": "#ffffff"
};

function ansiToHtml(text) {
    let result = "";
    let i = 0;
    let openSpan = false;
    while (i < text.length) {
        if (text[i] === "\x1b" && text[i + 1] === "[") {
            let j = i + 2;
            let code = "";
            while (j < text.length && text[j] !== "m") {
                code += text[j];
                j++;
            }
            if (j < text.length) {
                j++;
                const codes = code.split(";");
                let color = null;
                let bold = false;
                for (const c of codes) {
                    if (c === "0" || c === "") {
                        if (openSpan) { result += "</span>"; openSpan = false; }
                    } else if (c === "1") {
                        bold = true;
                    } else if (ANSI_COLORS[c]) {
                        color = ANSI_COLORS[c];
                    }
                }
                if (color) {
                    if (openSpan) result += "</span>";
                    const style = "color:" + color + (bold ? ";font-weight:bold" : "");
                    result += '<span style="' + style + '">';
                    openSpan = true;
                }
                i = j;
                continue;
            }
        }
        const ch = text[i];
        if (ch === "<") result += "&lt;";
        else if (ch === ">") result += "&gt;";
        else if (ch === "&") result += "&amp;";
        else result += ch;
        i++;
    }
    if (openSpan) result += "</span>";
    return result;
}

function printLine(text) {
    if (text === "\x1b[clear]") {
        output.innerHTML = "";
        return;
    }
    const div = document.createElement("div");
    if (text.includes("\x1b[")) {
        div.innerHTML = ansiToHtml(text);
    } else {
        div.textContent = text;
    }
    output.appendChild(div);
    terminal.scrollTop = terminal.scrollHeight;
}

let ws = null;
let wsReconnectTimer = null;

function connectWebSocket() {
    const proto = location.protocol === "https:" ? "wss:" : "ws:";
    const host = location.hostname || "localhost";
    const port = location.port;
    let wsUrl;
    if (!port || port === "80" || port === "443") {
        wsUrl = proto + "//" + host + "/ws";
    } else {
        wsUrl = "ws://" + host + ":8080";
    }
    ws = new WebSocket(wsUrl);

    ws.onopen = function () {
        console.log("[ZeroRing] WebSocket connected to", wsUrl);
        ws.send(JSON.stringify({ session: sessionId }));
    };

    ws.onmessage = function (e) {
        if (typeof e.data === "string" && e.data.startsWith("__session__")) {
            sessionId = e.data.slice(11);
            localStorage.setItem("zeroring_session", sessionId);
            return;
        }
        if (typeof e.data === "string" && e.data.startsWith("__complete__")) {
            handleTabResponse(e.data.slice(12));
            return;
        }
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
            const match = currentPrompt.match(/zeroring:(.+)\$\s*$/);
            if (match) {
                currentCwd = match[1];
            }
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

function handleTabResponse(jsonArray) {
    if (pendingTabPrefix === null) return;
    const prefix = pendingTabPrefix;
    pendingTabPrefix = null;
    let entries;
    try {
        entries = JSON.parse(jsonArray);
    } catch {
        return;
    }
    const matches = entries.filter(function (name) {
        return name.startsWith(prefix);
    });
    if (matches.length === 0) return;
    if (matches.length === 1) {
        const completion = matches[0].slice(prefix.length);
        const currentText = cmdInput.textContent;
        cmdInput.textContent = currentText + completion;
        syncWasmLine(cmdInput.textContent);
    } else {
        let common = matches[0];
        for (let i = 1; i < matches.length; i++) {
            while (!matches[i].startsWith(common)) {
                common = common.slice(0, -1);
            }
        }
        if (common.length > prefix.length) {
            const completion = common.slice(prefix.length);
            const currentText = cmdInput.textContent;
            cmdInput.textContent = currentText + completion;
            syncWasmLine(cmdInput.textContent);
        } else {
            printLine(matches.join("  "));
        }
    }
}

document.addEventListener("keydown", function (e) {
    if (!wasmInstance) return;

    if (e.key === "Tab") {
        e.preventDefault();
        const text = cmdInput.textContent;
        const parts = text.split(/\s+/);
        const lastWord = parts[parts.length - 1] || "";
        pendingTabPrefix = lastWord;
        if (ws && ws.readyState === WebSocket.OPEN) {
            ws.send(JSON.stringify({ cmd: "complete", path: currentCwd }));
        }
        return;
    }

    if (e.key === "ArrowUp") {
        e.preventDefault();
        if (commandHistory.length === 0) return;
        if (historyIndex === -1) {
            savedInput = cmdInput.textContent;
        }
        if (historyIndex < commandHistory.length - 1) {
            historyIndex++;
        }
        const cmd = commandHistory[commandHistory.length - 1 - historyIndex];
        cmdInput.textContent = cmd;
        syncWasmLine(cmd);
        return;
    }

    if (e.key === "ArrowDown") {
        e.preventDefault();
        if (historyIndex === -1) return;
        historyIndex--;
        let cmd;
        if (historyIndex === -1) {
            cmd = savedInput;
        } else {
            cmd = commandHistory[commandHistory.length - 1 - historyIndex];
        }
        cmdInput.textContent = cmd;
        syncWasmLine(cmd);
        return;
    }

    if (e.key === "Enter") {
        const cmd = cmdInput.textContent;
        printLine(currentPrompt + cmd);
        if (cmd.trim().length > 0) {
            commandHistory.push(cmd);
        }
        historyIndex = -1;
        savedInput = "";
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

    if (e.ctrlKey && e.key === "c") {
        const selection = window.getSelection().toString();
        if (selection) {
            navigator.clipboard.writeText(selection);
        } else {
            navigator.clipboard.writeText(cmdInput.textContent);
        }
        e.preventDefault();
        return;
    }

    if (e.ctrlKey && e.key === "v") {
        e.preventDefault();
        navigator.clipboard.readText().then(function (text) {
            const clean = text.replace(/[\r\n]/g, "");
            cmdInput.textContent += clean;
            syncWasmLine(cmdInput.textContent);
        });
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
