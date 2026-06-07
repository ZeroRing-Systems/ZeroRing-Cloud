#include <iostream>
#include <cstring>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "websocket.h"
#include "db_manager.h"

static DBManager db;
static std::mutex db_mutex;

static int create_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "bind failed on port " << port << "\n";
        close(fd);
        std::exit(1);
    }
    if (listen(fd, 4) < 0) {
        std::cerr << "listen failed\n";
        close(fd);
        std::exit(1);
    }
    return fd;
}

static bool do_handshake(int client) {
    char buf[4096]{};
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

static std::string extract_json_value(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    auto pos = json.find(search);
    if (pos == std::string::npos) {
        search = "\"" + key + "\":";
        pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        auto end = pos;
        while (end < json.size() && json[end] != ',' && json[end] != '}') end++;
        return json.substr(pos, end - pos);
    }
    pos += search.size();
    auto end = json.find("\"", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static std::string handle_syscall(const std::string& payload) {
    std::string action = extract_json_value(payload, "action");
    std::string file = extract_json_value(payload, "file");

    std::lock_guard<std::mutex> lock(db_mutex);

    if (action == "read_file") {
        if (!db.file_exists(file))
            return "{\"status\":\"error\",\"message\":\"not found\"}";
        std::string content = db.read_file(file);
        return "{\"status\":\"ok\",\"data\":\"" + content + "\"}";
    }

    if (action == "write_file") {
        std::string data = extract_json_value(payload, "data");
        db.save_file(file, data);
        return "{\"status\":\"ok\"}";
    }

    if (action == "delete_file") {
        if (!db.delete_file(file))
            return "{\"status\":\"error\",\"message\":\"not found\"}";
        return "{\"status\":\"ok\"}";
    }

    if (action == "list_files") {
        auto files = db.list_files();
        std::string arr = "[";
        for (size_t i = 0; i < files.size(); i++) {
            if (i) arr += ",";
            arr += "\"" + files[i] + "\"";
        }
        arr += "]";
        return "{\"status\":\"ok\",\"files\":" + arr + "}";
    }

    if (action == "open") {
        std::string flags_str = extract_json_value(payload, "flags");
        int flags = 0;
        if (!flags_str.empty()) flags = std::stoi(flags_str);
        int fd = db.open_file(file, flags);
        if (fd < 0)
            return "{\"status\":\"error\",\"message\":\"cannot open\"}";
        return "{\"status\":\"ok\",\"fd\":" + std::to_string(fd) + "}";
    }

    if (action == "fd_read") {
        std::string fd_str = extract_json_value(payload, "fd");
        if (fd_str.empty())
            return "{\"status\":\"error\",\"message\":\"missing fd\"}";
        int fd = std::stoi(fd_str);
        if (!db.fd_valid(fd))
            return "{\"status\":\"error\",\"message\":\"bad fd\"}";
        std::string content = db.fd_read(fd);
        return "{\"status\":\"ok\",\"data\":\"" + content + "\"}";
    }

    if (action == "fd_write") {
        std::string fd_str = extract_json_value(payload, "fd");
        if (fd_str.empty())
            return "{\"status\":\"error\",\"message\":\"missing fd\"}";
        int fd = std::stoi(fd_str);
        std::string data = extract_json_value(payload, "data");
        int n = db.fd_write(fd, data);
        if (n < 0)
            return "{\"status\":\"error\",\"message\":\"bad fd\"}";
        return "{\"status\":\"ok\",\"bytes\":" + std::to_string(n) + "}";
    }

    if (action == "close") {
        std::string fd_str = extract_json_value(payload, "fd");
        if (fd_str.empty())
            return "{\"status\":\"error\",\"message\":\"missing fd\"}";
        int fd = std::stoi(fd_str);
        if (!db.fd_close(fd))
            return "{\"status\":\"error\",\"message\":\"bad fd\"}";
        return "{\"status\":\"ok\"}";
    }

    return "{\"status\":\"error\",\"message\":\"unknown action\"}";
}

static void handle_client(int client) {
    if (!do_handshake(client)) {
        close(client);
        return;
    }

    while (true) {
        ws::Frame frame = ws::decode_frame(client);

        if (frame.opcode == 0x8 || frame.opcode == 0x0)
            break;

        if (frame.opcode == 0x1) {
            std::string response = handle_syscall(frame.payload);
            ws::send_frame(client, 0x1, response);
        }

        if (frame.opcode == 0x9)
            ws::send_frame(client, 0xA, frame.payload);
    }

    close(client);
}

int main() {
    int port = 8080;
    int listener = create_listener(port);

    db.connect();
    db.save_file("config.sys", "SHELL=zerosh\\nHOSTNAME=zeroring\\nVERSION=0.1");
    db.save_file("motd.txt", "Welcome to ZeroRing OS");

    std::cout << "ws://localhost:" << port << "\n";

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client = accept(listener, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client < 0) continue;
        std::thread(handle_client, client).detach();
    }

    close(listener);
}