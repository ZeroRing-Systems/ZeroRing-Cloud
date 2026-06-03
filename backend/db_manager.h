#pragma once
#include <string>
#include <unordered_map>
#include <iostream>

struct DBManager {
    std::unordered_map<std::string, std::string> store;

    void connect() {
        std::cout << "[Database] Virtual filesystem initialized\n";
    }

    bool save_file(const std::string& path, const std::string& data) {
        store[path] = data;
        std::cout << "[Database] Wrote " << data.size() << " bytes to " << path << "\n";
        return true;
    }

    std::string read_file(const std::string& path) {
        auto it = store.find(path);
        if (it == store.end()) return "";
        std::cout << "[Database] Read " << path << "\n";
        return it->second;
    }

    bool file_exists(const std::string& path) {
        return store.count(path) > 0;
    }

    bool delete_file(const std::string& path) {
        auto it = store.find(path);
        if (it == store.end()) return false;
        store.erase(it);
        std::cout << "[Database] Deleted " << path << "\n";
        return true;
    }

    std::string list_files() {
        std::string result = "[";
        bool first = true;
        for (auto& [k, v] : store) {
            if (!first) result += ",";
            result += "\"" + k + "\"";
            first = false;
        }
        result += "]";
        return result;
    }
};
