#include <iostream>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "websocket.h"
#include "db_manager.h"

static DBManager db;

static int create_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    bind(fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(fd, 4);
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

static void handle_client(int client) {
    if (!do_handshake(client)) {
        close(client);
        return;
    }

    while (true) {
        ws::Frame frame = ws::decode_frame(client);
        if (frame.opcode == 0x8 || frame.opcode == 0x0) break;

        if (frame.opcode == 0x1) {
            ws::send_frame(client, 0x1, "{\"status\":\"ok\"}");
        }
    }

    close(client);
}

int main() {
    int port = 8080;
    int listener = create_listener(port);
    db.connect();

    std::cout << "ws://localhost:" << port << "\n";

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client = accept(listener, (struct sockaddr*)&client_addr, &client_len);
        if (client < 0) continue;
        std::thread(handle_client, client).detach();
    }
}