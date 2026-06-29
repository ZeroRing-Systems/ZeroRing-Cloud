#pragma once
#include <string>
#include <vector>

struct VFSEntry {
    std::string name;
    bool        is_dir;
    int64_t     size;
};

class DBManager {
public:
    DBManager();
    ~DBManager();

    bool connect(const std::string& conninfo = "");
    void disconnect();
    bool is_connected() const;

    bool init_schema();

    std::vector<VFSEntry> list_dir(const std::string& path);
    bool make_dir(const std::string& path);
    std::string read_file(const std::string& path);
    bool write_file(const std::string& path, const std::string& data);
    bool remove(const std::string& path);
    bool exists(const std::string& path);

private:
    struct Impl;
    Impl* impl_;
};
