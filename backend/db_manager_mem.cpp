#ifndef USE_POSTGRES

#include "db_manager.h"
#include <map>
#include <iostream>
#include <mutex>

struct VFSNode {
    std::string name;
    bool        is_dir;
    std::string data;
    std::map<std::string, VFSNode*> children;

    VFSNode(const std::string& n, bool dir) : name(n), is_dir(dir) {}

    ~VFSNode() {
        for (auto& [k, v] : children) delete v;
    }
};

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
    VFSNode* root;
    std::mutex mtx;

    Impl() : root(new VFSNode("/", true)) {}
    ~Impl() { delete root; }

    VFSNode* resolve(const std::string& path) {
        auto parts = path_util::split(path);
        VFSNode* cur = root;
        for (auto& p : parts) {
            auto it = cur->children.find(p);
            if (it == cur->children.end()) return nullptr;
            cur = it->second;
        }
        return cur;
    }

    VFSNode* resolve_parent(const std::string& path, std::string& out_name) {
        out_name = path_util::basename(path);
        std::string par = path_util::parent(path);
        VFSNode* node = resolve(par);
        if (node && node->is_dir) return node;
        return nullptr;
    }
};

DBManager::DBManager() : impl_(new Impl()) {}

DBManager::~DBManager() {
    delete impl_;
}

bool DBManager::connect(const std::string&) {
    std::cerr << "[db] using in-memory filesystem (no PostgreSQL)\n";
    return true;
}

void DBManager::disconnect() {}

bool DBManager::is_connected() const { return true; }

bool DBManager::init_schema() {
    std::cerr << "[db] in-memory schema ready\n";
    return true;
}

std::vector<VFSEntry> DBManager::list_dir(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::vector<VFSEntry> entries;
    VFSNode* node = impl_->resolve(path);
    if (!node || !node->is_dir) return entries;

    for (auto& [name, child] : node->children) {
        VFSEntry e;
        e.name   = name;
        e.is_dir = child->is_dir;
        e.size   = child->is_dir ? -1 : static_cast<int64_t>(child->data.size());
        entries.push_back(e);
    }
    return entries;
}

bool DBManager::make_dir(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::string name;
    VFSNode* parent = impl_->resolve_parent(path, name);
    if (!parent) return false;
    if (parent->children.count(name)) return false;

    parent->children[name] = new VFSNode(name, true);
    return true;
}

std::string DBManager::read_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    VFSNode* node = impl_->resolve(path);
    if (!node || node->is_dir) return "";
    return node->data;
}

bool DBManager::write_file(const std::string& path, const std::string& data) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::string name;
    VFSNode* parent = impl_->resolve_parent(path, name);
    if (!parent) return false;

    auto it = parent->children.find(name);
    if (it != parent->children.end()) {
        if (it->second->is_dir) return false;
        it->second->data = data;
    } else {
        auto* node = new VFSNode(name, false);
        node->data = data;
        parent->children[name] = node;
    }
    return true;
}

bool DBManager::remove(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    std::string name;
    VFSNode* parent = impl_->resolve_parent(path, name);
    if (!parent) return false;

    auto it = parent->children.find(name);
    if (it == parent->children.end()) return false;

    if (it->second->is_dir && !it->second->children.empty()) return false;

    delete it->second;
    parent->children.erase(it);
    return true;
}

bool DBManager::exists(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    return impl_->resolve(path) != nullptr;
}

bool DBManager::register_user(const std::string& username, const std::string& password) {
    return false; // Not supported in memory mode
}

bool DBManager::authenticate_user(const std::string& username, const std::string& password) {
    return false; // Not supported in memory mode
}

void DBManager::migrate_session_to_user(const std::string& session_id, const std::string& username) {
    // Not supported in memory mode
}

#endif
