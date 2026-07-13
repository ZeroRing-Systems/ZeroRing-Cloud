const terminal = document.getElementById("terminal");
const output = document.getElementById("output");
const inputLine = document.getElementById("input-line");
const promptEl = document.getElementById("prompt");
const cmdInput = document.getElementById("cmd");

let mem;
let wasmInstance;
let currentPrompt = "$ ";

function formatPromptHTML(promptStr) {
    // Example: "zeroring:/path$ "
    const match = promptStr.match(/^(.*?):(.*)\$\s*$/);
    if (match) {
        return `<span class="prompt-user">${match[1]}</span><span class="prompt-colon">:</span><span class="prompt-path">${match[2]}</span><span class="prompt-dollar">$ </span>`;
    }
    return promptStr;
}

const commandHistory = [];
let historyIndex = -1;
let savedInput = "";
let pendingTabPrefix = null;
let currentCwd = "/";
let sessionId = localStorage.getItem("zeroring_session") || "";

function generateSessionId() {
  const arr = new Uint8Array(16);
  crypto.getRandomValues(arr);
  return Array.from(arr, function (b) {
    return b.toString(16).padStart(2, "0");
  }).join("");
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

function writeCStr(jsString) {
  if (!wasmInstance || !wasmInstance.exports.js_scratch_buf) return 0;
  const ptr = wasmInstance.exports.js_scratch_buf.value;
  const bytes = new Uint8Array(mem.buffer);
  for (let i = 0; i < jsString.length && i < 4095; i++) {
    bytes[ptr + i] = jsString.charCodeAt(i);
  }
  bytes[ptr + Math.min(jsString.length, 4095)] = 0;
  return ptr;
}

function syncWasmLine(text) {
  if (!wasmInstance || !wasmInstance.exports.handle_key) return;
  wasmInstance.exports.handle_key(21);
  for (let i = 0; i < text.length; i++) {
    wasmInstance.exports.handle_key(text.charCodeAt(i));
  }
}

const ANSI_COLORS = {
  30: "#1e1e1e",
  31: "#e06c75",
  32: "#98c379",
  33: "#e5c07b",
  34: "#61afef",
  35: "#c678dd",
  36: "#56b6c2",
  37: "#abb2bf",
  90: "#5c6370",
  91: "#e06c75",
  92: "#98c379",
  93: "#e5c07b",
  94: "#61afef",
  95: "#c678dd",
  96: "#56b6c2",
  97: "#ffffff",
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
            if (openSpan) {
              result += "</span>";
              openSpan = false;
            }
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

const BACKEND_WS_URL = "wss://zeroring-sys.duckdns.org";

function connectWebSocket() {
  let wsUrl;
  if (location.protocol === "file:") {
    wsUrl = "ws://localhost:8080";
  } else {
    wsUrl = BACKEND_WS_URL;
  }
  ws = new WebSocket(wsUrl);

  ws.onopen = function () {
    console.log("[ZeroRing] WebSocket connected to", wsUrl);
    ws.send(JSON.stringify({ session: sessionId }));
  };

  let currentEditPath = null;

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
    if (typeof e.data === "string" && e.data.startsWith("__edit__")) {
      const nlIndex = e.data.indexOf("\n");
      const path = e.data.substring(8, nlIndex);
      const content = e.data.substring(nlIndex + 1);

      document.getElementById("editor-filename").textContent = path;
      const ext = path.split('.').pop().toLowerCase();
      let lang = "plaintext";
      if (ext === "js") lang = "javascript";
      else if (ext === "py") lang = "python";
      else if (ext === "cpp" || ext === "c" || ext === "h") lang = "cpp";
      else if (ext === "html") lang = "html";
      else if (ext === "css") lang = "css";
      else if (ext === "json") lang = "json";
      else if (ext === "sh") lang = "shell";

      if (window.monacoEditor) {
        window.monacoEditor.setValue(content);
        monaco.editor.setModelLanguage(window.monacoEditor.getModel(), lang);
      }
      document.getElementById("editor-overlay").style.display = "flex";
      if (window.monacoEditor) {
        window.monacoEditor.focus();
      }
      currentEditPath = path;
      return;
    }
    if (typeof e.data === "string" && e.data.startsWith("__download__")) {
      const nlIndex = e.data.indexOf("\n");
      const path = e.data.substring(12, nlIndex);
      const content = e.data.substring(nlIndex + 1);

      const filename = path.split("/").pop();
      const blob = new Blob([content], { type: "text/plain" });
      const url = window.URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.style.display = "none";
      a.href = url;
      a.download = filename;
      document.body.appendChild(a);
      a.click();
      window.URL.revokeObjectURL(url);
      document.body.removeChild(a);
      printLine("\x1b[32mdownloaded: " + path + "\x1b[0m");
      return;
    }
    if (typeof e.data === "string" && e.data.startsWith("__upload__")) {
      const path = e.data.slice(10);
      const input = document.createElement("input");
      input.type = "file";
      input.onchange = (ev) => {
        const file = ev.target.files[0];
        if (!file) return;
        const reader = new FileReader();
        reader.onload = (readEv) => {
          const content = readEv.target.result;
          ws.send(
            JSON.stringify({
              cmd: "upload",
              path: path,
              data: content,
            }),
          );
        };
        reader.readAsText(file); // Note: assumes text files for now
      };
      input.click();
      return;
    }
    
    if (typeof e.data === "string" && e.data.startsWith("__chat__")) {
      try {
        const chat = JSON.parse(e.data.substring(8));
        handleChatMessage(chat);
      } catch(err) {
        console.error("Failed to parse chat:", err);
      }
      return;
    }
    
    if (typeof e.data === "string" && e.data.startsWith("__notify__")) {
      const html = e.data.substring(10);
      showToast("📁 File Shared", html);
      return;
    }
    
    if (wasmInstance && wasmInstance.exports.handle_net_response) {
      const ptr = writeCStr(e.data);
      wasmInstance.exports.handle_net_response(ptr);
    } else {
      printLine(e.data);
    }
  };

  document.getElementById("editor-cancel").onclick = function () {
    document.getElementById("editor-overlay").style.display = "none";
    document.getElementById("terminal").focus();
  };

  document.getElementById("editor-save").onclick = function () {
    if (!currentEditPath) return;
    const content = window.monacoEditor ? window.monacoEditor.getValue() : "";
    ws.send(
      JSON.stringify({
        cmd: "save",
        path: currentEditPath,
        data: content,
      }),
    );
    document.getElementById("editor-overlay").style.display = "none";
    document.getElementById("terminal").focus();
    printLine("saved: " + currentEditPath + " (" + content.length + " bytes)");
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
      promptEl.innerHTML = formatPromptHTML(currentPrompt);
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
  if (e.target.tagName === "TEXTAREA" || e.target.tagName === "INPUT") return;
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
    
    const echoDiv = document.createElement("div");
    echoDiv.className = "echo-line";
    echoDiv.innerHTML = formatPromptHTML(currentPrompt) + `<span class="command-text">${cmd}</span>`;
    output.appendChild(echoDiv);
    terminal.scrollTop = terminal.scrollHeight;
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

terminal.addEventListener("dragover", function (e) {
  e.preventDefault();
  document.getElementById("drag-overlay").style.display = "block";
});

terminal.addEventListener("dragleave", function (e) {
  e.preventDefault();
  document.getElementById("drag-overlay").style.display = "none";
});

terminal.addEventListener("drop", function (e) {
  e.preventDefault();
  document.getElementById("drag-overlay").style.display = "none";
  if (e.dataTransfer.files.length > 0) {
    const file = e.dataTransfer.files[0];
    const reader = new FileReader();
    reader.onload = (evt) => {
      const content = evt.target.result;
      const path =
        currentCwd === "/" ? "/" + file.name : currentCwd + "/" + file.name;
      if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(
          JSON.stringify({
            cmd: "upload",
            path: path,
            data: content,
          }),
        );
      }
    };
    reader.readAsText(file);
  }
});

WebAssembly.instantiateStreaming(
  fetch("wasm/kernel.wasm?v=" + Date.now()),
  imports,
)
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

function showToast(title, message, onClick) {
  const container = document.getElementById("toast-container");
  if (!container) return;
  const toast = document.createElement("div");
  toast.style.background = "rgba(20, 20, 25, 0.95)";
  toast.style.border = "1px solid rgba(255, 255, 255, 0.1)";
  toast.style.borderLeft = title.includes("Private") ? "4px solid #fbcfe8" : "4px solid #818cf8";
  toast.style.padding = "12px 16px";
  toast.style.borderRadius = "8px";
  toast.style.color = "#fff";
  toast.style.boxShadow = "0 10px 30px rgba(0,0,0,0.5)";
  toast.style.animation = "slideIn 0.3s ease-out";
  toast.style.fontFamily = "'Outfit', sans-serif";
  toast.style.pointerEvents = "auto";
  toast.style.cursor = "pointer";
  toast.innerHTML = `<div style="font-weight: 600; margin-bottom: 4px; font-size: 0.9rem; color: #a5b4fc;">${title}</div>
                     <div style="font-size: 0.9rem; color: #e2e8f0; word-break: break-word;">${message}</div>`;
  
  toast.addEventListener("click", () => {
    toast.remove();
    if (onClick) onClick();
    else openChatPanel();
  });
  
  container.appendChild(toast);
  
  setTimeout(() => {
    toast.style.animation = "slideOut 0.3s ease-in forwards";
    setTimeout(() => toast.remove(), 300);
  }, 5000);
}

// ========== Chat Panel System ==========
const chatHistory = {}; // channel -> [{from, msg, time, isMine}]
let currentChannel = "global";
let currentUsername = "anonymous";
let totalUnread = 0;
const channelUnread = {};

function getMyUsername() {
  // Try to extract from prompt text
  const promptEl = document.getElementById("prompt");
  if (promptEl) {
    const text = promptEl.textContent || promptEl.innerText || "";
    const match = text.match(/^([a-zA-Z0-9_]+)/);
    if (match && match[1] !== "zeroring") {
      currentUsername = match[1];
    }
  }
  return currentUsername;
}

function handleChatMessage(chat) {
  const { from, target, msg, sid } = chat;
  
  // Use session ID to determine if this is our own message
  const isMine = (sid === sessionId);
  // Determine which channel this belongs to
  let channel;
  if (target === "global") {
    channel = "global";
  } else {
    // Private message - channel is the other person's name
    channel = isMine ? target : from;
  }
  
  // Ensure channel exists
  if (!chatHistory[channel]) {
    chatHistory[channel] = [];
    addChannelTab(channel);
  }
  
  const now = new Date();
  const timeStr = now.getHours().toString().padStart(2, "0") + ":" + now.getMinutes().toString().padStart(2, "0");
  
  chatHistory[channel].push({ from, msg, time: timeStr, isMine });
  
  // If viewing this channel, render it
  if (currentChannel === channel && document.getElementById("chat-panel").classList.contains("open")) {
    renderMessages();
  } else {
    // Increment unread
    channelUnread[channel] = (channelUnread[channel] || 0) + 1;
    totalUnread++;
    updateBadges();
    
    // Show toast if panel closed or different channel
    if (!isMine) {
      const isPrivate = target !== "global";
      const title = isPrivate 
        ? `🔒 Private from <b>${from}</b>` 
        : `🌐 <b>${from}</b>`;
      showToast(title, msg, () => {
        openChatPanel();
        switchChannel(channel);
      });
    }
  }
}

function addChannelTab(channel) {
  const container = document.getElementById("chat-channels");
  // Check if tab already exists
  const existing = container.querySelector(`[data-channel="${channel}"]`);
  if (existing) return;
  
  const btn = document.createElement("button");
  btn.className = "channel-tab";
  btn.dataset.channel = channel;
  btn.onclick = () => switchChannel(channel);
  const icon = channel === "global" ? "🌐" : "👤";
  btn.innerHTML = `${icon} ${channel}<span class="unread" style="display:none"></span>`;
  container.appendChild(btn);
}

function switchChannel(channel) {
  currentChannel = channel;
  
  // Update active tab
  document.querySelectorAll(".channel-tab").forEach(tab => {
    tab.classList.toggle("active", tab.dataset.channel === channel);
  });
  
  // Clear unread for this channel
  if (channelUnread[channel]) {
    totalUnread -= channelUnread[channel];
    if (totalUnread < 0) totalUnread = 0;
    channelUnread[channel] = 0;
    updateBadges();
  }
  
  renderMessages();
  
  // Update placeholder
  const input = document.getElementById("chat-input");
  if (channel === "global") {
    input.placeholder = "Message #global...";
  } else {
    input.placeholder = `Message @${channel}...`;
  }
}

function renderMessages() {
  const container = document.getElementById("chat-messages");
  const msgs = chatHistory[currentChannel] || [];
  
  if (msgs.length === 0) {
    container.innerHTML = '<div class="chat-empty">No messages yet.<br>Start a conversation!</div>';
    return;
  }
  
  container.innerHTML = msgs.map(m => `
    <div class="chat-msg ${m.isMine ? 'sent' : 'received'}">
      <div class="msg-sender">${m.isMine ? 'You' : m.from}</div>
      <div>${escapeHtml(m.msg)}</div>
      <div class="msg-time">${m.time}</div>
    </div>
  `).join("");
  
  container.scrollTop = container.scrollHeight;
}

function escapeHtml(text) {
  const div = document.createElement("div");
  div.textContent = text;
  return div.innerHTML;
}

function updateBadges() {
  // Title bar badge
  const mainBadge = document.getElementById("chat-badge");
  if (totalUnread > 0) {
    mainBadge.textContent = totalUnread;
    mainBadge.style.display = "inline-block";
  } else {
    mainBadge.style.display = "none";
  }
  
  // Per-channel badges
  document.querySelectorAll(".channel-tab").forEach(tab => {
    const ch = tab.dataset.channel;
    const badge = tab.querySelector(".unread");
    if (badge && channelUnread[ch] && channelUnread[ch] > 0) {
      badge.textContent = channelUnread[ch];
      badge.style.display = "inline-block";
    } else if (badge) {
      badge.style.display = "none";
    }
  });
}

function toggleChatPanel() {
  const panel = document.getElementById("chat-panel");
  panel.classList.toggle("open");
  if (panel.classList.contains("open")) {
    // Clear unread for current channel
    if (channelUnread[currentChannel]) {
      totalUnread -= channelUnread[currentChannel];
      if (totalUnread < 0) totalUnread = 0;
      channelUnread[currentChannel] = 0;
      updateBadges();
    }
    renderMessages();
    document.getElementById("chat-input").focus();
  }
}

function openChatPanel() {
  const panel = document.getElementById("chat-panel");
  if (!panel.classList.contains("open")) {
    panel.classList.add("open");
  }
  renderMessages();
  document.getElementById("chat-input").focus();
}

function sendChatFromPanel() {
  const input = document.getElementById("chat-input");
  const msg = input.value.trim();
  if (!msg || !ws || ws.readyState !== 1) return;
  
  let payload;
  if (currentChannel === "global") {
    payload = msg;
  } else {
    payload = `@${currentChannel} ${msg}`;
  }
  
  ws.send(JSON.stringify({ cmd: "chat", path: payload }));
  input.value = "";
  input.focus();
}

// Chat input Enter key
document.getElementById("chat-input").addEventListener("keydown", function(e) {
  if (e.key === "Enter") {
    e.preventDefault();
    sendChatFromPanel();
  }
  e.stopPropagation(); // Don't let terminal capture these keystrokes
});

// Also stop keyup/keypress from bubbling
document.getElementById("chat-input").addEventListener("keyup", e => e.stopPropagation());
document.getElementById("chat-input").addEventListener("keypress", e => e.stopPropagation());

// ========== Monaco Editor Setup ==========
window.monacoEditor = null;
require.config({ paths: { 'vs': 'https://cdnjs.cloudflare.com/ajax/libs/monaco-editor/0.41.0/min/vs' }});
require(['vs/editor/editor.main'], function() {
    window.monacoEditor = monaco.editor.create(document.getElementById('editor-container'), {
        value: "",
        language: "plaintext",
        theme: "vs-dark",
        automaticLayout: true,
        minimap: { enabled: false },
        fontSize: 14,
        fontFamily: "'JetBrains Mono', monospace"
    });
});


