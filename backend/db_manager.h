#pragma once
#include <string>
#include <map>
#include <vector>

struct DBManager {
    std::map<std::string, std::string> store;

    struct OpenFile {
        std::string path;
        int flags;
    };
    std::map<int, OpenFile> fds;
    int next_fd = 3;

    void connect() {}

    bool save_file(const std::string& path, const std::string& data) {
        store[path] = data;
        return true;
    }

    std::string read_file(const std::string& path) {
        if (store.find(path) == store.end()) return "";
        return store[path];
    }

    bool file_exists(const std::string& path) {
        return store.find(path) != store.end();
    }

    bool delete_file(const std::string& path) {
        if (store.find(path) == store.end()) return false;
        store.erase(path);
        return true;
    }

    std::vector<std::string> list_files() {
        std::vector<std::string> out;
        for (std::map<std::string, std::string>::iterator it = store.begin();
             it != store.end(); it++) {
            out.push_back(it->first);
        }
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
        if (fds.find(fd) == fds.end()) return "";
        return read_file(fds[fd].path);
    }

    int fd_write(int fd, const std::string& data) {
        if (fds.find(fd) == fds.end()) return -1;
        save_file(fds[fd].path, data);
        return data.size();
    }

    bool fd_close(int fd) {
        return fds.erase(fd) > 0;
    }

    bool fd_valid(int fd) {
        return fds.find(fd) != fds.end();
    }
};
