#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

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
        close(client);
    }

    close(listener);
}