#ifdef USE_POSTGRES

#include "db_manager.h"
#include <pqxx/pqxx>
#include <iostream>
#include <sstream>
#include <mutex>

namespace path_util {

static std::vector<std::string> split(const std::string& path) {
    std::vector<std::string> parts;
    std::string segment;
    for (char c : path) {
        if (c == '/') {
            if (!segment.empty()) { parts.push_back(segment); segment.clear(); }
        } else {
            segment += c;
        }
    }
    if (!segment.empty()) parts.push_back(segment);
    return parts;
}

static std::string parent(const std::string& path) {
    if (path == "/" || path.empty()) return "/";
    auto pos = path.rfind('/');
    if (pos == 0 || pos == std::string::npos) return "/";
    return path.substr(0, pos);
}

static std::string basename(const std::string& path) {
    auto pos = path.rfind('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

}

struct DBManager::Impl {
    std::unique_ptr<pqxx::connection> conn;
    std::mutex mtx;

    int64_t resolve_dir(pqxx::work& txn, const std::string& path) {
        auto parts = path_util::split(path);
        int64_t dir_id = 1;
        for (auto& part : parts) {
            auto r = txn.exec_params(
                "SELECT id FROM directories WHERE parent_id = $1 AND name = $2",
                dir_id, part
            );
            if (r.empty()) return -1;
            dir_id = r[0][0].as<int64_t>();
        }
        return dir_id;
    }
};

DBManager::DBManager() : impl_(new Impl()) {}

DBManager::~DBManager() {
    disconnect();
    delete impl_;
}

bool DBManager::connect(const std::string& conninfo) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    try {
        std::string ci = conninfo.empty()
            ? "host=localhost port=5432 dbname=zeroring user=zeroring password=zeroring"
            : conninfo;
        impl_->conn = std::make_unique<pqxx::connection>(ci);
        std::cerr << "[db] connected to PostgreSQL\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[db] connection failed: " << e.what() << "\n";
        return false;
    }
}

void DBManager::disconnect() {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    impl_->conn.reset();
}

bool DBManager::is_connected() const {
    return impl_->conn && impl_->conn->is_open();
}

bool DBManager::init_schema() {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    try {
        pqxx::work txn(*impl_->conn);
        txn.exec(R"SQL(
            CREATE TABLE IF NOT EXISTS users (
                id          SERIAL PRIMARY KEY,
                username    VARCHAR(64) UNIQUE NOT NULL,
                created_at  TIMESTAMPTZ DEFAULT NOW()
            );
            CREATE TABLE IF NOT EXISTS directories (
                id          SERIAL PRIMARY KEY,
                parent_id   INTEGER REFERENCES directories(id) ON DELETE CASCADE,
                owner_id    INTEGER REFERENCES users(id) DEFAULT 1,
                name        VARCHAR(255) NOT NULL,
                created_at  TIMESTAMPTZ DEFAULT NOW(),
                UNIQUE(parent_id, name)
            );
            CREATE TABLE IF NOT EXISTS files (
                id          SERIAL PRIMARY KEY,
                directory_id INTEGER NOT NULL REFERENCES directories(id) ON DELETE CASCADE,
                owner_id    INTEGER REFERENCES users(id) DEFAULT 1,
                name        VARCHAR(255) NOT NULL,
                data        BYTEA DEFAULT '',
                size        BIGINT DEFAULT 0,
                created_at  TIMESTAMPTZ DEFAULT NOW(),
                updated_at  TIMESTAMPTZ DEFAULT NOW(),
                UNIQUE(directory_id, name)
            );
            INSERT INTO users (id, username) VALUES (1, 'root')
                ON CONFLICT DO NOTHING;
            INSERT INTO directories (id, parent_id, name) VALUES (1, NULL, '/')
                ON CONFLICT DO NOTHING;
        )SQL");
        txn.commit();
        std::cerr << "[db] schema initialized\n";
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[db] schema init failed: " << e.what() << "\n";
        return false;
    }
}

std::vector<VFSEntry> DBManager::list_dir(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::vector<VFSEntry> entries;
    try {
        pqxx::work txn(*impl_->conn);
        int64_t dir_id = impl_->resolve_dir(txn, path);
        if (dir_id < 0) return entries;
        auto dirs = txn.exec_params(
            "SELECT name FROM directories WHERE parent_id = $1 ORDER BY name", dir_id);
        for (auto& row : dirs)
            entries.push_back({row[0].as<std::string>(), true, -1});
        auto files = txn.exec_params(
            "SELECT name, size FROM files WHERE directory_id = $1 ORDER BY name", dir_id);
        for (auto& row : files)
            entries.push_back({row[0].as<std::string>(), false, row[1].as<int64_t>()});
        txn.commit();
    } catch (const std::exception& e) {
        std::cerr << "[db] list_dir error: " << e.what() << "\n";
    }
    return entries;
}

bool DBManager::make_dir(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    try {
        pqxx::work txn(*impl_->conn);
        std::string parent = path_util::parent(path);
        std::string name = path_util::basename(path);
        int64_t parent_id = impl_->resolve_dir(txn, parent);
        if (parent_id < 0) return false;
        txn.exec_params(
            "INSERT INTO directories (parent_id, name) VALUES ($1, $2) ON CONFLICT DO NOTHING",
            parent_id, name);
        txn.commit();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[db] mkdir error: " << e.what() << "\n";
        return false;
    }
}

std::string DBManager::read_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    try {
        pqxx::work txn(*impl_->conn);
        std::string dir = path_util::parent(path);
        std::string name = path_util::basename(path);
        int64_t dir_id = impl_->resolve_dir(txn, dir);
        if (dir_id < 0) return "";
        auto r = txn.exec_params(
            "SELECT data FROM files WHERE directory_id = $1 AND name = $2", dir_id, name);
        txn.commit();
        if (r.empty()) return "";
        auto bv = r[0][0].as<pqxx::binarystring>();
        return std::string(reinterpret_cast<const char*>(bv.data()), bv.size());
    } catch (const std::exception& e) {
        std::cerr << "[db] read_file error: " << e.what() << "\n";
        return "";
    }
}

bool DBManager::write_file(const std::string& path, const std::string& data) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    try {
        pqxx::work txn(*impl_->conn);
        std::string dir = path_util::parent(path);
        std::string name = path_util::basename(path);
        int64_t dir_id = impl_->resolve_dir(txn, dir);
        if (dir_id < 0) return false;
        pqxx::binarystring blob(data.data(), data.size());
        txn.exec_params(
            "INSERT INTO files (directory_id, name, data, size) "
            "VALUES ($1, $2, $3, $4) "
            "ON CONFLICT (directory_id, name) DO UPDATE "
            "SET data = EXCLUDED.data, size = EXCLUDED.size, updated_at = NOW()",
            dir_id, name, blob, static_cast<int64_t>(data.size()));
        txn.commit();
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[db] write_file error: " << e.what() << "\n";
        return false;
    }
}

bool DBManager::remove(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    try {
        pqxx::work txn(*impl_->conn);
        std::string dir = path_util::parent(path);
        std::string name = path_util::basename(path);
        int64_t dir_id = impl_->resolve_dir(txn, dir);
        if (dir_id < 0) return false;
        auto rf = txn.exec_params(
            "DELETE FROM files WHERE directory_id = $1 AND name = $2", dir_id, name);
        if (rf.affected_rows() > 0) { txn.commit(); return true; }
        auto rd = txn.exec_params(
            "DELETE FROM directories WHERE parent_id = $1 AND name = $2 "
            "AND NOT EXISTS (SELECT 1 FROM directories d2 WHERE d2.parent_id = directories.id) "
            "AND NOT EXISTS (SELECT 1 FROM files f WHERE f.directory_id = directories.id)",
            dir_id, name);
        txn.commit();
        return rd.affected_rows() > 0;
    } catch (const std::exception& e) {
        std::cerr << "[db] remove error: " << e.what() << "\n";
        return false;
    }
}

bool DBManager::exists(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    try {
        pqxx::work txn(*impl_->conn);
        if (impl_->resolve_dir(txn, path) >= 0) { txn.commit(); return true; }
        std::string dir = path_util::parent(path);
        std::string name = path_util::basename(path);
        int64_t dir_id = impl_->resolve_dir(txn, dir);
        if (dir_id < 0) { txn.commit(); return false; }
        auto r = txn.exec_params(
            "SELECT 1 FROM files WHERE directory_id = $1 AND name = $2", dir_id, name);
        txn.commit();
        return !r.empty();
    } catch (const std::exception& e) {
        return false;
    }
}

#endif
