#pragma once
#include <string>
#include <vector>

struct VFSEntry {
    std::string name;
    bool        is_dir;
    int64_t     size;
    int         permissions = 420; // 0644 octal = 420 decimal
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
    bool rename(const std::string& old_path, const std::string& new_path);
    bool copy(const std::string& old_path, const std::string& new_path);
    bool chmod(const std::string& path, int permissions);
    int get_permissions(const std::string& path);

    // User Management
    bool register_user(const std::string& username, const std::string& password);
    bool authenticate_user(const std::string& username, const std::string& password);
    void migrate_session_to_user(const std::string& session_id, const std::string& username);

private:
    struct Impl;
    Impl* impl_;
};
