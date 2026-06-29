// ============================================================================
// db_manager.h — Virtual Filesystem Database Interface
// ============================================================================
// Abstracts the persistent storage layer for ZeroRing-Cloud. The virtual
// filesystem is stored in PostgreSQL with a hierarchical schema:
//
//   users(id, username, created_at)
//   directories(id, parent_id, owner_id, name, created_at)
//   files(id, directory_id, owner_id, name, data, size, created_at, updated_at)
//
// When PostgreSQL is unavailable (compile with -DUSE_MEMORY_BACKEND), an
// in-memory tree is used instead. This allows development without a running
// database server.
// ============================================================================
#pragma once
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// VFS entry returned by listing operations
// ---------------------------------------------------------------------------
struct VFSEntry {
    std::string name;
    bool        is_dir;
    int64_t     size;       // -1 for directories
};

// ---------------------------------------------------------------------------
// DBManager — Virtual Filesystem operations
// ---------------------------------------------------------------------------
class DBManager {
public:
    DBManager();
    ~DBManager();

    // Lifecycle
    bool connect(const std::string& conninfo = "");
    void disconnect();
    bool is_connected() const;

    // Schema bootstrap — creates tables if they don't exist
    bool init_schema();

    // --------------- Filesystem operations ---------------

    // List entries in a directory. Returns empty vector on error.
    std::vector<VFSEntry> list_dir(const std::string& path);

    // Create a directory. Returns true on success.
    bool make_dir(const std::string& path);

    // Read file contents. Returns file data or empty string.
    std::string read_file(const std::string& path);

    // Write (create or overwrite) a file with the given data.
    bool write_file(const std::string& path, const std::string& data);

    // Remove a file or empty directory. Returns true on success.
    bool remove(const std::string& path);

    // Check if a path exists (file or directory).
    bool exists(const std::string& path);

private:
    struct Impl;
    Impl* impl_;
};
