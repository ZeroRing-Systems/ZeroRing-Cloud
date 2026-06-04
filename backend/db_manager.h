#pragma once
#include <string>
#include <unordered_map>
#include <vector>

struct DBManager {
    std::unordered_map<std::string, std::string> store;

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
};
