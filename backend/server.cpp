#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "websocket.h"

static int create_listener(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    listen(fd, 4);
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

static void handle_client(int client) {
    if (!do_handshake(client)) {
        std::cerr << "[Backend] Handshake failed\n";
        close(client);
        return;
    }

    std::cout << "[Backend] WebSocket handshake complete\n";

    while (true) {
        ws::Frame frame = ws::decode_frame(client);

        if (frame.opcode == 0x8 || frame.opcode == 0x0) {
            std::cout << "[Backend] Client disconnected\n";
            break;
        }

        if (frame.opcode == 0x1) {
            std::cout << "[Backend] Received: " << frame.payload << "\n";
            ws::send_frame(client, 0x1, "{\"status\":\"ok\"}");
        }

        if (frame.opcode == 0x9) {
            ws::send_frame(client, 0xA, frame.payload);
        }
    }

    close(client);
}

int main() {
    int port = 8080;
    int listener = create_listener(port);

    std::cout << "[Backend] ZeroRing Cloud Server listening on ws://localhost:" << port << "\n";

    while (true) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client = accept(listener, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

        if (client < 0) {
            std::cerr << "[Backend] Accept failed\n";
            continue;
        }

        std::cout << "[Backend] Client connected\n";
        handle_client(client);
    }

    close(listener);
}