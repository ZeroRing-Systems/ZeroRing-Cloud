#ifndef USE_POSTGRES

#include "db_manager.h"
#include <map>
#include <iostream>
#include <mutex>

struct VFSNode {
    std::string name;
    bool        is_dir;
    std::string data;
    int         permissions;
    std::map<std::string, VFSNode*> children;

    VFSNode(const std::string& n, bool dir) : name(n), is_dir(dir), permissions(dir ? 493 : 420) {}

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
    std::map<std::string, std::string> users;

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
        e.permissions = child->permissions;
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

bool DBManager::rename(const std::string& old_path, const std::string& new_path) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (old_path.empty() || new_path.empty() || old_path == "/" || new_path == "/" || old_path == new_path) return false;
    std::string old_name;
    VFSNode* old_parent = impl_->resolve_parent(old_path, old_name);
    if (!old_parent) return false;
    auto it = old_parent->children.find(old_name);
    if (it == old_parent->children.end()) return false;
    VFSNode* node = it->second;

    std::string target_path = new_path;
    VFSNode* target_node = impl_->resolve(new_path);
    if (target_node && target_node->is_dir) {
        target_path = new_path + "/" + old_name;
    }
    if (node->is_dir && (target_path == old_path || target_path.rfind(old_path + "/", 0) == 0)) {
        return false;
    }

    std::string new_name;
    VFSNode* new_parent = impl_->resolve_parent(target_path, new_name);
    if (!new_parent || !new_parent->is_dir) return false;

    auto dest_it = new_parent->children.find(new_name);
    if (dest_it != new_parent->children.end()) {
        if (dest_it->second->is_dir || node->is_dir) return false;
        delete dest_it->second;
        new_parent->children.erase(dest_it);
    }

    old_parent->children.erase(it);
    node->name = new_name;
    new_parent->children[new_name] = node;
    return true;
}

bool DBManager::copy(const std::string& old_path, const std::string& new_path) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (old_path.empty() || new_path.empty() || old_path == "/" || new_path == "/" || old_path == new_path) return false;
    VFSNode* node = impl_->resolve(old_path);
    if (!node || node->is_dir) return false;

    std::string target_path = new_path;
    VFSNode* target_node = impl_->resolve(new_path);
    if (target_node && target_node->is_dir) {
        target_path = new_path + "/" + node->name;
    }

    std::string new_name;
    VFSNode* new_parent = impl_->resolve_parent(target_path, new_name);
    if (!new_parent || !new_parent->is_dir) return false;

    auto dest_it = new_parent->children.find(new_name);
    if (dest_it != new_parent->children.end()) {
        if (dest_it->second->is_dir) return false;
        dest_it->second->data = node->data;
        dest_it->second->permissions = node->permissions;
        return true;
    }

    VFSNode* copy_node = new VFSNode(new_name, false);
    copy_node->data = node->data;
    copy_node->permissions = node->permissions;
    new_parent->children[new_name] = copy_node;
    return true;
}

bool DBManager::chmod(const std::string& path, int permissions) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    VFSNode* node = impl_->resolve(path);
    if (!node) return false;
    node->permissions = permissions;
    return true;
}

int DBManager::get_permissions(const std::string& path) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    VFSNode* node = impl_->resolve(path);
    if (!node) return -1;
    return node->permissions;
}

bool DBManager::register_user(const std::string& username, const std::string& password) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (impl_->users.count(username)) return false;
    impl_->users[username] = password;
    return true;
}

bool DBManager::authenticate_user(const std::string& username, const std::string& password) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    auto it = impl_->users.find(username);
    if (it != impl_->users.end() && it->second == password) return true;
    return false;
}

void DBManager::migrate_session_to_user(const std::string& session_id, const std::string& username) {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    VFSNode* sessions_dir = impl_->resolve("/sessions");
    if (!sessions_dir) return;

    auto it = sessions_dir->children.find(session_id);
    if (it == sessions_dir->children.end()) return;

    VFSNode* session_node = it->second;
    sessions_dir->children.erase(it);

    VFSNode* users_dir = impl_->resolve("/users");
    if (!users_dir) {
        users_dir = new VFSNode("users", true);
        impl_->root->children["users"] = users_dir;
    }

    // Rename the session node to the username
    session_node->name = username;
    
    // If the user already had a directory, we could merge or overwrite. 
    // Here we just overwrite for simplicity in memory mode.
    if (users_dir->children.count(username)) {
        delete users_dir->children[username];
    }
    users_dir->children[username] = session_node;
}

#endif
