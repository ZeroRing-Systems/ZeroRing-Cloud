#pragma once
#include <string>
#include <unordered_map>
#include <vector>

struct DBManager {
    std::unordered_map<std::string, std::string> store;

    struct OpenFile {
        std::string path;
        int flags;
    };
    std::unordered_map<int, OpenFile> fds;
    int next_fd = 3;

    void connect() {}

    bool save_file(const std::string& path, const std::string& data) {
        store[path] = data;
        return true;
    }

    std::string read_file(const std::string& path) {
        auto it = store.find(path);
        if (it == store.end()) return "";
        return it->second;
    }

    bool file_exists(const std::string& path) {
        return store.count(path) > 0;
    }

    bool delete_file(const std::string& path) {
        auto it = store.find(path);
        if (it == store.end()) return false;
        store.erase(it);
        return true;
    }

    std::vector<std::string> list_files() {
        std::vector<std::string> out;
        out.reserve(store.size());
        for (auto& [k, _] : store)
            out.push_back(k);
        return out;
    }

    int open_file(const std::string& path, int flags) {
        if (flags == 1 && !file_exists(path))
            save_file(path, "");
        else if (!file_exists(path))
            return -1;
        int fd = next_fd++;
        fds[fd] = {path, flags};
        return fd;
    }

    std::string fd_read(int fd) {
        auto it = fds.find(fd);
        if (it == fds.end()) return "";
        return read_file(it->second.path);
    }

    int fd_write(int fd, const std::string& data) {
        auto it = fds.find(fd);
        if (it == fds.end()) return -1;
        save_file(it->second.path, data);
        return data.size();
    }

    bool fd_close(int fd) {
        return fds.erase(fd) > 0;
    }

    bool fd_valid(int fd) {
        return fds.count(fd) > 0;
    }
};
