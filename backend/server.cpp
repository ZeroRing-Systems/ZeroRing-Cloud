#include <iostream>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <iomanip>

#include "websocket.h"
#include "db_manager.h"
#include "json_util.h"

static DBManager db;
static std::map<std::string, bool> known_sessions;
static std::mutex sessions_mtx;

static std::string generate_session_id() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << dis(gen);
    return ss.str();
}

static void ensure_session_root(const std::string& session_id) {
    std::string session_root = "/sessions/" + session_id;
    if (!db.exists("/sessions")) {
        db.make_dir("/sessions");
    }
    if (!db.exists(session_root)) {
        db.make_dir(session_root);
    }
}

static std::string scope_path(const std::string& session_id, const std::string& path) {
    std::string base = "/sessions/" + session_id;
    if (path == "/") return base;
    if (path[0] == '/') return base + path;
    return base + "/" + path;
}

static int create_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "[server] socket() failed\n";
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[server] bind() failed on port " << port << "\n";
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        std::cerr << "[server] listen() failed\n";
        close(fd);
        return -1;
    }

    return fd;
}

static bool do_handshake(int client) {
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    int n = recv(client, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return false;

    std::string request(buf, n);
    std::string key = ws::find_header(request, "Sec-WebSocket-Key");
    if (key.empty()) return false;

    std::string accept = ws::compute_accept_key(key);
    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

    send(client, response.c_str(), response.size(), 0);
    return true;
}

static std::string format_ls(const std::vector<VFSEntry>& entries) {
    if (entries.empty()) return "(empty directory)";

    std::string out;
    for (auto& e : entries) {
        if (e.is_dir) {
            out += "\033[1;34m" + e.name + "/\033[0m\n";
        } else {
            out += "\033[37m" + e.name;
            if (e.size >= 0) {
                out += "  \033[90m(" + std::to_string(e.size) + " bytes)\033[0m";
            }
            out += "\033[0m\n";
        }
    }
    if (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

static std::string route_command(const std::string& raw, const std::string& session_id) {
    auto obj = json::parse(raw);

    auto it_cmd = obj.find("cmd");
    if (it_cmd == obj.end()) {
        return json::error("missing 'cmd' field");
    }
    const std::string& cmd = it_cmd->second;

    if (cmd == "ping") {
        return json::ok("pong");
    }

    if (cmd == "ls") {
        std::string path = obj.count("path") ? obj["path"] : "/";
        std::string scoped = scope_path(session_id, path);
        std::cerr << "[debug] ls: path=" << path << " scoped=" << scoped << "\n";
        auto entries = db.list_dir(scoped);
        std::cerr << "[debug] ls: found " << entries.size() << " entries\n";
        return format_ls(entries);
    }

    if (cmd == "complete") {
        std::string path = obj.count("path") ? obj["path"] : "/";
        auto entries = db.list_dir(scope_path(session_id, path));
        std::string out = "__complete__[";
        for (size_t i = 0; i < entries.size(); i++) {
            if (i > 0) out += ",";
            out += "\"" + entries[i].name;
            if (entries[i].is_dir) out += "/";
            out += "\"";
        }
        out += "]";
        return out;
    }

    if (cmd == "stat") {
        if (!obj.count("path")) return "__stat__notfound";
        std::string scoped = scope_path(session_id, obj["path"]);
        if (!db.exists(scoped)) return "__stat__notfound";
        auto entries = db.list_dir(scoped);
        if (entries.size() > 0 || db.read_file(scoped).empty()) {
            return "__stat__dir";
        }
        return "__stat__file";
    }

    if (cmd == "mkdir") {
        if (!obj.count("path")) return json::error("mkdir: missing 'path'");
        std::string scoped = scope_path(session_id, obj["path"]);
        std::cerr << "[debug] mkdir: path=" << obj["path"] << " scoped=" << scoped << "\n";
        if (db.make_dir(scoped)) {
            return "mkdir: created " + obj["path"];
        }
        return "mkdir: failed to create " + obj["path"];
    }

    if (cmd == "cat") {
        if (!obj.count("path")) return json::error("cat: missing 'path'");
        std::string content = db.read_file(scope_path(session_id, obj["path"]));
        if (content.empty()) {
            return "cat: " + obj["path"] + ": no such file";
        }
        return content;
    }

    if (cmd == "save") {
        if (!obj.count("path")) return json::error("save: missing 'path'");
        std::string data = obj.count("data") ? obj["data"] : "";
        if (db.write_file(scope_path(session_id, obj["path"]), data)) {
            return "saved: " + obj["path"] + " (" + std::to_string(data.size()) + " bytes)";
        }
        return "save: failed to write " + obj["path"];
    }

    if (cmd == "rm") {
        if (!obj.count("path")) return json::error("rm: missing 'path'");
        if (db.remove(scope_path(session_id, obj["path"]))) {
            return "rm: removed " + obj["path"];
        }
        return "rm: failed to remove " + obj["path"] + " (not found or not empty)";
    }

    return "unknown command: " + cmd;
}

static void handle_client(int client) {
    if (!do_handshake(client)) {
        close(client);
        return;
    }

    ws::Frame auth_frame = ws::decode_frame(client);
    if (auth_frame.opcode != 0x1) {
        close(client);
        return;
    }

    auto auth_obj = json::parse(auth_frame.payload);
    std::string session_id;

    if (auth_obj.count("session")) {
        session_id = auth_obj["session"];
    }

    if (session_id.empty()) {
        session_id = generate_session_id();
    }

    {
        std::lock_guard<std::mutex> lock(sessions_mtx);
        if (!known_sessions.count(session_id)) {
            known_sessions[session_id] = true;
        }
    }

    ensure_session_root(session_id);

    ws::send_frame(client, 0x1, "__session__" + session_id);

    std::cerr << "[server] client connected (fd=" << client << " session=" << session_id << ")\n";

    while (true) {
        ws::Frame frame = ws::decode_frame(client);

        if (frame.opcode == 0x8 || frame.opcode == 0x0) break;

        if (frame.opcode == 0x1) {
            std::string response = route_command(frame.payload, session_id);
            ws::send_frame(client, 0x1, response);
        }

        if (frame.opcode == 0x9) {
            ws::send_frame(client, 0xA, frame.payload);
        }
    }

    std::cerr << "[server] client disconnected (fd=" << client << ")\n";
    close(client);
}

int main() {
    const int port = 8080;

    std::string conninfo;
    const char* env = std::getenv("ZERORING_DB");
    if (env) conninfo = env;

    if (!db.connect(conninfo)) {
        std::cerr << "[server] WARNING: database connection failed, using in-memory VFS\n";
    }
    db.init_schema();

    int listener = create_listener(port);
    if (listener < 0) return 1;

    std::cerr << "[server] listening on ws://localhost:" << port << "\n";

    while (true) {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client = accept(listener, (struct sockaddr*)&client_addr, &client_len);
        if (client < 0) continue;
        std::thread(handle_client, client).detach();
    }
}