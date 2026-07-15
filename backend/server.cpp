#include "db_manager.h"
#include "json_util.h"
#include "websocket.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <netinet/in.h>
#include <random>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static DBManager db;
static std::map<std::string, bool> known_sessions;
static std::map<std::string, std::string> session_to_user; // session_id -> username
static std::mutex sessions_mtx;

#include <algorithm>
#include <functional>
#include <map>
static std::map<int, std::string> active_clients;
static std::mutex clients_mtx;

static std::string generate_session_id()
{
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << dis(gen);
    return ss.str();
}

// Security: validate that a session ID contains only safe hex characters
static bool is_valid_session_id(const std::string& sid)
{
    if (sid.empty() || sid.size() > 32) return false;
    for (char c : sid)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return true;
}

static void ensure_session_root(const std::string& session_id)
{
    std::string user = "";
    {
        std::lock_guard<std::mutex> lock(sessions_mtx);
        auto it = session_to_user.find(session_id);
        if (it != session_to_user.end())
            user = it->second;
    }

    if (!user.empty())
    {
        std::string user_root = "/users/" + user;
        if (!db.exists("/users"))
            db.make_dir("/users");
        if (!db.exists(user_root))
            db.make_dir(user_root);
    }
    else
    {
        std::string session_root = "/sessions/" + session_id;
        if (!db.exists("/sessions"))
            db.make_dir("/sessions");
        if (!db.exists(session_root))
            db.make_dir(session_root);
    }
}

static std::string scope_path(const std::string& session_id, const std::string& path)
{
    std::string user = "";
    {
        std::lock_guard<std::mutex> lock(sessions_mtx);
        auto it = session_to_user.find(session_id);
        if (it != session_to_user.end())
            user = it->second;
    }

    std::string base = user.empty() ? ("/sessions/" + session_id) : ("/users/" + user);

    // Security: normalize path to prevent ../ traversal
    std::string normalized;
    std::vector<std::string> parts;
    std::string segment;
    std::string full = path;
    for (size_t i = 0; i < full.size(); i++)
    {
        if (full[i] == '/')
        {
            if (segment == "..")
            {
                if (!parts.empty()) parts.pop_back();
            }
            else if (!segment.empty() && segment != ".")
            {
                parts.push_back(segment);
            }
            segment.clear();
        }
        else
        {
            segment += full[i];
        }
    }
    if (segment == "..")
    {
        if (!parts.empty()) parts.pop_back();
    }
    else if (!segment.empty() && segment != ".")
    {
        parts.push_back(segment);
    }

    normalized = base;
    for (auto& p : parts)
        normalized += "/" + p;

    if (normalized.empty()) normalized = base;
    return normalized;
}

static int create_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        std::cerr << "[server] socket() failed\n";
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        std::cerr << "[server] bind() failed on port " << port << "\n";
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0)
    {
        std::cerr << "[server] listen() failed\n";
        close(fd);
        return -1;
    }

    return fd;
}

static bool do_handshake(int client)
{
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    int n = recv(client, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
        return false;

    std::string request(buf, n);
    std::string key = ws::find_header(request, "Sec-WebSocket-Key");
    if (key.empty())
        return false;

    std::string accept = ws::compute_accept_key(key);
    std::string response = "HTTP/1.1 101 Switching Protocols\r\n"
                           "Upgrade: websocket\r\n"
                           "Connection: Upgrade\r\n"
                           "Sec-WebSocket-Accept: " +
                           accept + "\r\n\r\n";

    send(client, response.c_str(), response.size(), 0);
    return true;
}

static bool wildcard_match(const char* pattern, const char* str)
{
    const char *s = str, *p = pattern;
    const char *star = nullptr, *ss = str;
    while (*s)
    {
        if (*p == *s || *p == '?')
        {
            p++;
            s++;
        }
        else if (*p == '*')
        {
            star = p++;
            ss = s;
        }
        else if (star)
        {
            p = star + 1;
            s = ++ss;
        }
        else
        {
            return false;
        }
    }
    while (*p == '*')
        p++;
    return *p == '\0';
}

static std::string get_basename(const std::string& path)
{
    if (path.empty() || path == "/")
        return "/";
    auto p = path.find_last_of('/');
    if (p == std::string::npos)
        return path;
    if (p == path.size() - 1)
    {
        auto p2 = path.find_last_of('/', p - 1);
        if (p2 == std::string::npos)
            return path.substr(0, p);
        return path.substr(p2 + 1, p - p2 - 1);
    }
    return path.substr(p + 1);
}

static std::string mode_to_string(bool is_dir, int permissions)
{
    std::string s = is_dir ? "d" : "-";
    s += (permissions & 0400) ? "r" : "-";
    s += (permissions & 0200) ? "w" : "-";
    s += (permissions & 0100) ? "x" : "-";
    s += (permissions & 0040) ? "r" : "-";
    s += (permissions & 0020) ? "w" : "-";
    s += (permissions & 0010) ? "x" : "-";
    s += (permissions & 0004) ? "r" : "-";
    s += (permissions & 0002) ? "w" : "-";
    s += (permissions & 0001) ? "x" : "-";
    return s;
}

static std::string format_ls(const std::vector<VFSEntry>& entries, bool long_format = false)
{
    if (entries.empty())
        return "(empty directory)";

    std::string out;
    for (auto& e : entries)
    {
        if (long_format)
        {
            std::string mode_str = mode_to_string(e.is_dir, e.permissions);
            if (e.is_dir)
            {
                out += "\033[90m" + mode_str + "\033[0m  \033[1;34m" + e.name + "/\033[0m\n";
            }
            else
            {
                out += "\033[90m" + mode_str + "\033[0m  \033[37m" + e.name + "\033[0m";
                if (e.size >= 0)
                {
                    out += "  \033[90m(" + std::to_string(e.size) + " bytes)\033[0m";
                }
                out += "\n";
            }
        }
        else
        {
            if (e.is_dir)
            {
                out += "\033[1;34m" + e.name + "/\033[0m\n";
            }
            else
            {
                out += "\033[37m" + e.name;
                if (e.size >= 0)
                {
                    out += "  \033[90m(" + std::to_string(e.size) + " bytes)\033[0m";
                }
                out += "\n";
            }
        }
    }
    if (!out.empty() && out.back() == '\n')
        out.pop_back();
    return out;
}

static std::string
route_command(const std::string& raw, const std::string& session_id, int client_fd)
{
    auto obj = json::parse(raw);

    auto it_cmd = obj.find("cmd");
    if (it_cmd == obj.end())
    {
        return json::error("missing 'cmd' field");
    }
    const std::string& cmd = it_cmd->second;

    if (cmd == "ping")
    {
        return json::ok("pong");
    }

    if (cmd == "echo")
    {
        return obj.count("path") ? obj["path"] : "";
    }

    if (cmd == "ls")
    {
        std::string path = obj.count("path") ? obj["path"] : "/";
        bool long_format = false;
        if (obj.count("mode") &&
            (obj["mode"] == "-l" || obj["mode"] == "-la" || obj["mode"] == "-al"))
            long_format = true;
        if (path == "-l" || path == "-la" || path == "-al")
        {
            long_format = true;
            path = "/";
        }
        else if (path.find("-l ") == 0 || path.find("-la ") == 0 || path.find("-al ") == 0)
        {
            long_format = true;
            auto sp = path.find(' ');
            path = path.substr(sp + 1);
        }
        std::string scoped = scope_path(session_id, path);
        std::cerr << "[debug] ls: path=" << path << " scoped=" << scoped << "\n";

        // Auto-create 'shared' directory so it is visible in the root
        if (path == "/")
        {
            std::string shared_dir = scoped + "/shared";
            if (!db.exists(shared_dir))
            {
                db.make_dir(shared_dir);
            }
        }

        auto entries = db.list_dir(scoped);
        std::cerr << "[debug] ls: found " << entries.size() << " entries\n";

        return format_ls(entries, long_format);
    }

    if (cmd == "find")
    {
        std::string path = obj.count("path") ? obj["path"] : "/";
        std::string name_pat = obj.count("name") ? obj["name"] : "";
        std::string scoped_root = scope_path(session_id, path);
        if (!db.exists(scoped_root))
            return "find: '" + path + "': No such file or directory";

        std::vector<std::string> results;
        std::function<void(const std::string&, const std::string&, int)> find_rec =
            [&](const std::string& cur_scoped, const std::string& cur_display, int depth)
        {
            if (depth > 30)
                return;
            auto entries = db.list_dir(cur_scoped);
            for (const auto& e : entries)
            {
                std::string next_scoped = (cur_scoped == "/" ? "" : cur_scoped) + "/" + e.name;
                std::string next_display = (cur_display == "/" ? "" : cur_display) + "/" + e.name;
                if (cur_display == ".")
                    next_display = "./" + e.name;
                else if (cur_display == "..")
                    next_display = "../" + e.name;

                if (name_pat.empty() || wildcard_match(name_pat.c_str(), e.name.c_str()))
                {
                    results.push_back(next_display);
                }
                if (e.is_dir)
                {
                    find_rec(next_scoped, next_display, depth + 1);
                }
            }
        };

        std::string initial_display = path;
        if (name_pat.empty() || wildcard_match(name_pat.c_str(), get_basename(path).c_str()))
        {
            results.push_back(initial_display);
        }
        find_rec(scoped_root, initial_display, 0);

        std::string out = "";
        for (size_t i = 0; i < results.size(); i++)
        {
            if (i > 0)
                out += "\n";
            out += results[i];
        }
        return out;
    }

    if (cmd == "tree")
    {
        std::string path = obj.count("path") ? obj["path"] : "/";
        std::string scoped_root = scope_path(session_id, path);
        if (!db.exists(scoped_root))
            return "tree: '" + path + "': No such file or directory";

        int dir_count = 0;
        int file_count = 0;
        std::string out = path == "/" ? "/" : get_basename(path);

        std::function<void(const std::string&, const std::string&, int)> tree_rec =
            [&](const std::string& cur_scoped, const std::string& prefix, int depth)
        {
            if (depth > 25)
            {
                out += "\n" + prefix + "└── ... (max depth reached)";
                return;
            }
            auto entries = db.list_dir(cur_scoped);
            for (size_t i = 0; i < entries.size(); i++)
            {
                const auto& e = entries[i];
                bool is_last = (i == entries.size() - 1);
                out += "\n" + prefix + (is_last ? "└── " : "├── ") + e.name;
                if (e.is_dir)
                {
                    dir_count++;
                    std::string next_scoped = (cur_scoped == "/" ? "" : cur_scoped) + "/" + e.name;
                    tree_rec(next_scoped, prefix + (is_last ? "    " : "│   "), depth + 1);
                }
                else
                {
                    file_count++;
                }
            }
        };

        tree_rec(scoped_root, "", 0);
        out += "\n\n" + std::to_string(dir_count) + " director" + (dir_count == 1 ? "y" : "ies") +
               ", " + std::to_string(file_count) + " file" + (file_count == 1 ? "" : "s");
        return out;
    }

    if (cmd == "chmod")
    {
        if (!obj.count("path") || !obj.count("mode"))
            return json::error("chmod: missing 'path' or 'mode'");
        std::string path = obj["path"];
        std::string mode = obj["mode"];
        std::string scoped = scope_path(session_id, path);
        if (!db.exists(scoped))
            return "chmod: cannot access '" + path + "': No such file or directory";

        int cur_perms = db.get_permissions(scoped);
        if (cur_perms < 0)
            cur_perms = 0644;

        int new_perms = cur_perms;
        if (mode == "+x")
            new_perms |= 0111;
        else if (mode == "-x")
            new_perms &= ~0111;
        else if (mode == "+r")
            new_perms |= 0444;
        else if (mode == "-r")
            new_perms &= ~0444;
        else if (mode == "+w")
            new_perms |= 0222;
        else if (mode == "-w")
            new_perms &= ~0222;
        else if (mode == "u+x")
            new_perms |= 0100;
        else if (mode == "u-x")
            new_perms &= ~0100;
        else if (mode == "g+x")
            new_perms |= 0010;
        else if (mode == "g-x")
            new_perms &= ~0010;
        else if (mode == "o+x")
            new_perms |= 0001;
        else if (mode == "o-x")
            new_perms &= ~0001;
        else if (mode == "a+x")
            new_perms |= 0111;
        else if (mode == "a-x")
            new_perms &= ~0111;
        else
        {
            try
            {
                new_perms = std::stoi(mode, nullptr, 8);
            }
            catch (...)
            {
                return "chmod: invalid mode: '" + mode + "'";
            }
        }

        if (db.chmod(scoped, new_perms))
            return "chmod: mode of '" + path + "' changed to " + mode_to_string(false, new_perms);
        return "chmod: failed to change permissions of '" + path + "'";
    }

    if (cmd == "complete")
    {
        std::string path = obj.count("path") ? obj["path"] : "/";
        auto entries = db.list_dir(scope_path(session_id, path));
        std::string out = "__complete__[";
        for (size_t i = 0; i < entries.size(); i++)
        {
            if (i > 0)
                out += ",";
            out += "\"" + entries[i].name;
            if (entries[i].is_dir)
                out += "/";
            out += "\"";
        }
        out += "]";
        return out;
    }

    if (cmd == "stat")
    {
        if (!obj.count("path"))
            return "__stat__notfound";
        std::string scoped = scope_path(session_id, obj["path"]);
        if (!db.exists(scoped))
            return "__stat__notfound";
        auto entries = db.list_dir(scoped);
        if (entries.size() > 0 || db.read_file(scoped).empty())
        {
            return "__stat__dir";
        }
        return "__stat__file";
    }

    if (cmd == "mkdir")
    {
        if (!obj.count("path"))
            return json::error("mkdir: missing 'path'");
        std::string scoped = scope_path(session_id, obj["path"]);
        std::cerr << "[debug] mkdir: path=" << obj["path"] << " scoped=" << scoped << "\n";
        if (db.make_dir(scoped))
        {
            return "mkdir: created " + obj["path"];
        }
        return "mkdir: failed to create " + obj["path"];
    }

    if (cmd == "touch")
    {
        if (!obj.count("path"))
            return json::error("touch: missing 'path'");
        std::string scoped = scope_path(session_id, obj["path"]);
        if (!db.exists(scoped))
        {
            if (db.write_file(scoped, ""))
                return "";
            return "touch: failed to create " + obj["path"];
        }
        return ""; // Success quietly
    }

    if (cmd == "cat")
    {
        if (!obj.count("path"))
            return json::error("cat: missing 'path'");
        std::string content = db.read_file(scope_path(session_id, obj["path"]));
        if (content.empty())
        {
            return "cat: " + obj["path"] + ": no such file";
        }
        return content;
    }

    if (cmd == "save")
    {
        if (!obj.count("path"))
            return json::error("save: missing 'path'");
        std::string data = obj.count("data") ? obj["data"] : "";
        if (db.write_file(scope_path(session_id, obj["path"]), data))
        {
            return "saved: " + obj["path"] + " (" + std::to_string(data.size()) + " bytes)";
        }
        return "save: failed to write " + obj["path"];
    }

    if (cmd == "rm")
    {
        if (!obj.count("path"))
            return json::error("rm: missing 'path'");
        if (db.remove(scope_path(session_id, obj["path"])))
        {
            return "rm: removed " + obj["path"];
        }
        return "rm: failed to remove " + obj["path"] + " (not found or not empty)";
    }

    if (cmd == "mv")
    {
        if (!obj.count("src") || !obj.count("dest"))
            return json::error("mv: missing 'src' or 'dest'");
        std::string scoped_src = scope_path(session_id, obj["src"]);
        std::string scoped_dest = scope_path(session_id, obj["dest"]);
        if (db.rename(scoped_src, scoped_dest))
        {
            return "mv: moved " + obj["src"] + " -> " + obj["dest"];
        }
        return "mv: failed to move '" + obj["src"] + "' to '" + obj["dest"] + "' (check paths)";
    }

    if (cmd == "cp")
    {
        if (!obj.count("src") || !obj.count("dest"))
            return json::error("cp: missing 'src' or 'dest'");
        std::string scoped_src = scope_path(session_id, obj["src"]);
        std::string scoped_dest = scope_path(session_id, obj["dest"]);
        if (db.copy(scoped_src, scoped_dest))
        {
            return "cp: copied " + obj["src"] + " -> " + obj["dest"];
        }
        return "cp: failed to copy '" + obj["src"] + "' to '" + obj["dest"] + "' (check paths)";
    }

    if (cmd == "edit")
    {
        if (!obj.count("path"))
            return json::error("edit: missing 'path'");
        std::string content = db.read_file(scope_path(session_id, obj["path"]));
        return "__edit__" + obj["path"] + "\n" + content;
    }

    if (cmd == "run")
    {
        if (!obj.count("path"))
            return json::error("run: missing 'path'");
        std::string scoped = scope_path(session_id, obj["path"]);
        int perms = db.get_permissions(scoped);
        if (perms >= 0 && !(perms & 0111))
        {
            return "run: permission denied: '" + obj["path"] +
                   "' is not marked executable (use `chmod +x " + obj["path"] + "`)";
        }
        std::string content = db.read_file(scoped);
        if (content.empty())
            return "run: file not found or empty";

        // Detect language by file extension
        std::string path = obj["path"];
        std::string ext = "";
        auto dot = path.rfind('.');
        if (dot != std::string::npos)
            ext = path.substr(dot);

        struct LangConfig
        {
            bool is_compiled;
            std::string image;
            std::string command;
        };

        std::map<std::string, LangConfig> dispatch = {
            {".py", {false, "python:3.9-slim", "python3 /app/script.py"}},
            {".js", {false, "node:18-slim", "node /app/script.js"}},
            {".sh", {false, "ubuntu:22.04", "bash /app/script.sh"}},
            {".cpp", {true, "gcc:13", "cd /app && g++ script.cpp -o out && ./out"}},
            {".rs", {true, "rust:slim", "cd /app && rustc script.rs -o out && ./out"}},
            {".go", {true, "golang:1.21-alpine", "cd /app && go run script.go"}}};

        if (dispatch.find(ext) == dispatch.end())
            return "run: unsupported file type '" + ext +
                   "' (supported: .py, .js, .sh, .cpp, .rs, .go)";

        auto cfg = dispatch[ext];

        // Create temporary directory using safe mkdtemp instead of system()
        char tmpl[] = "/tmp/zr_run_XXXXXX";
        char* tmp_result = mkdtemp(tmpl);
        if (!tmp_result)
            return "run: failed to create temp directory";
        std::string tmp_dir(tmp_result);
        ::chmod(tmp_dir.c_str(), 0777);

        std::string temp_file = tmp_dir + "/script" + ext;
        FILE* f = fopen(temp_file.c_str(), "w");
        if (!f)
        {
            rmdir(tmp_dir.c_str());
            return "run: failed to create temp file";
        }
        fwrite(content.data(), 1, content.size(), f);
        fclose(f);
        ::chmod(temp_file.c_str(), 0666);

        // Execute in strict ephemeral Docker container with timeout + fork bomb protection
        std::string mount_type = cfg.is_compiled ? ":/app" : ":/app:ro";
        std::string cmd_str = "docker run --rm --network none -m 128m --cpus=\"0.5\" --pids-limit 64 "
                              "-v " + tmp_dir + mount_type + " " +
                              cfg.image + " timeout 10s sh -c '" + cfg.command + "' 2>&1";

        FILE* pipe = popen(cmd_str.c_str(), "r");
        if (!pipe)
        {
            unlink((tmp_dir + "/script" + ext).c_str());
            if (!ext.empty()) unlink((tmp_dir + "/out").c_str());
            rmdir(tmp_dir.c_str());
            return "run: failed to execute";
        }

        char buffer[256];
        std::string result;
        size_t total_read = 0;
        const size_t MAX_OUTPUT = 8192; // Cap output at 8KB
        while (fgets(buffer, sizeof(buffer), pipe) != NULL && total_read < MAX_OUTPUT)
        {
            result += buffer;
            total_read += strlen(buffer);
        }
        int status = pclose(pipe);
        unlink((tmp_dir + "/script" + ext).c_str());
        unlink((tmp_dir + "/out").c_str());
        rmdir(tmp_dir.c_str());

        if (total_read >= MAX_OUTPUT)
            result += "\n[output truncated at 8KB]";
        if (WIFEXITED(status) && WEXITSTATUS(status) == 124)
            result += "\n[execution timed out after 10 seconds]";

        if (result.empty())
            return "run: success (no output)";
        return result;
    }

    // === Web Fetch / HTTP Client ===
    // fetch command REMOVED — it was an SSRF vector allowing attackers to
    // probe internal services (127.0.0.1, metadata endpoints, etc.)
    if (cmd == "fetch")
    {
        return "\033[31mError:\033[0m fetch command has been disabled for security reasons";
    }

    // === Upload (base64 file upload) ===
    if (cmd == "upload")
    {
        if (!obj.count("path"))
            return json::error("upload: missing 'path'");
        if (!obj.count("data"))
            return json::error("upload: missing 'data'");
        std::string data = obj["data"];
        std::string scoped = scope_path(session_id, obj["path"]);
        if (db.write_file(scoped, data))
        {
            return "uploaded: " + obj["path"] + " (" + std::to_string(data.size()) + " bytes)";
        }
        return "upload: failed to write " + obj["path"];
    }

    // === Download ===
    if (cmd == "download")
    {
        if (!obj.count("path"))
            return json::error("download: missing 'path'");
        std::string content = db.read_file(scope_path(session_id, obj["path"]));
        if (content.empty())
            return json::error("download: file not found");
        return "__download__" + obj["path"] + "\n" + content;
    }

    // === User Registration ===
    if (cmd == "register")
    {
        if (!obj.count("username") || !obj.count("password"))
            return "usage: register <username> <password>";
        std::string username = obj["username"];
        std::string password = obj["password"];
        if (username.size() < 3 || username.size() > 32)
            return "register: username must be 3-32 characters";
        if (password.size() < 4)
            return "register: password must be at least 4 characters";
        // Sanitize: alphanumeric + underscore only
        for (char c : username)
        {
            if (!isalnum(c) && c != '_')
                return "register: username can only contain letters, numbers, and underscores";
        }
        if (db.register_user(username, password))
        {
            // Auto-login after register
            {
                std::lock_guard<std::mutex> lock(sessions_mtx);
                session_to_user[session_id] = username;
            }
            ensure_session_root(session_id);
            return "__reset__\033[32mregistered and logged in as " + username + "\033[0m";
        }
        return "\033[31mregister: username '" + username + "' is already taken\033[0m";
    }

    // === User Login ===
    if (cmd == "login")
    {
        if (!obj.count("username") || !obj.count("password"))
            return "usage: login <username> <password>";
        std::string username = obj["username"];
        std::string password = obj["password"];
        if (db.authenticate_user(username, password))
        {
            {
                std::lock_guard<std::mutex> lock(sessions_mtx);
                session_to_user[session_id] = username;
            }
            ensure_session_root(session_id);
            return "__reset__\033[32mlogged in as " + username + "\033[0m";
        }
        return "\033[31mlogin: invalid username or password\033[0m";
    }

    // === Logout ===
    if (cmd == "logout")
    {
        std::lock_guard<std::mutex> lock(sessions_mtx);
        session_to_user.erase(session_id);
        return "__reset__logged out";
    }

    // === Who Am I (with user context) ===
    if (cmd == "whoami_user")
    {
        std::lock_guard<std::mutex> lock(sessions_mtx);
        auto it = session_to_user.find(session_id);
        if (it != session_to_user.end())
        {
            return "\033[96m" + it->second + "\033[0m";
        }
        return "\033[35manonymous\033[0m (session: " + session_id.substr(0, 8) + "...)";
    }

    // === Share a file ===
    if (cmd == "share")
    {
        if (!obj.count("path"))
            return json::error("share: missing 'path'");

        std::string path = obj["path"];

        std::string target_user = "";
        if (obj.count("target"))
            target_user = obj["target"];

        std::string actual_path = scope_path(session_id, path);
        if (!db.exists(actual_path))
            return "share: file not found: " + path;

        std::string content = db.read_file(actual_path);

        std::string filename = path;
        auto slash = path.rfind('/');
        if (slash != std::string::npos)
            filename = path.substr(slash + 1);

        if (!target_user.empty())
        {
            // Sanitize target_user to alphanumeric + underscore only
            for (char c : target_user)
            {
                if (!isalnum(c) && c != '_')
                    return "share: invalid target username";
            }
            std::string dest_dir = "/users/" + target_user + "/shared";
            if (!db.exists(dest_dir))
            {
                db.make_dir(dest_dir);
            }
            // Sanitize filename: strip path separators
            std::string safe_filename;
            for (char c : filename)
            {
                if (c != '/' && c != '\\' && c != '.') safe_filename += c;
                else if (c == '.' && !safe_filename.empty()) safe_filename += c;
            }
            if (safe_filename.empty()) safe_filename = "shared_file";
            std::string dest_path = dest_dir + "/" + safe_filename;

            db.write_file(dest_path, content);

            // Send notification to target user (escape HTML)
            std::string safe_user = session_to_user.count(session_id) ? session_to_user[session_id] : "anonymous";
            std::string notify_html;
            for (char c : safe_user)
            {
                if (c == '<') notify_html += "&lt;";
                else if (c == '>') notify_html += "&gt;";
                else if (c == '&') notify_html += "&amp;";
                else notify_html += c;
            }
            notify_html += " shared " + safe_filename + " with you!";
            std::vector<int> target_fds;
            {
                std::lock_guard<std::mutex> lock1(clients_mtx);
                std::lock_guard<std::mutex> lock2(sessions_mtx);
                for (auto const& [fd, s_id] : active_clients)
                {
                    auto it2 = session_to_user.find(s_id);
                    if (it2 != session_to_user.end() && it2->second == target_user)
                    {
                        target_fds.push_back(fd);
                    }
                }
            }
            std::string payload = "__notify__" + notify_html;
            for (int fd : target_fds)
            {
                ws::send_frame(fd, 0x1, payload);
            }

            return "\033[32mSuccess:\033[0m Privately shared " + path + " with @" + target_user +
                   ".";
        }
        else
        {
            // Global share
            if (!db.exists("/shared"))
            {
                db.make_dir("/shared");
            }
            if (db.write_file("/shared/" + filename, content))
            {
                return "published " + path + " -> global registry";
            }
            return "\033[31mError:\033[0m failed to write to global registry";
        }
    }

    // === Unshare a file ===
    if (cmd == "unshare")
    {
        std::string username = "anonymous";
        {
            std::lock_guard<std::mutex> lock(sessions_mtx);
            auto it = session_to_user.find(session_id);
            if (it != session_to_user.end())
                username = it->second;
        }

        if (username != "root")
            return "\033[31mError:\033[0m Permission denied. Only 'root' can delete globally "
                   "shared files.";

        if (!obj.count("path"))
            return json::error("unshare: missing 'path'");

        std::string path = obj["path"];
        std::string filename = path;
        auto slash = path.rfind('/');
        if (slash != std::string::npos)
            filename = path.substr(slash + 1);

        std::string shared_path = "/shared/" + filename;
        if (!db.exists(shared_path))
            return "unshare: file not found: " + filename;

        db.remove(shared_path);
        return "unshared " + filename + " from global registry";
    }

    // === ZPM Install ===
    if (cmd == "zpm_install")
    {
        if (!obj.count("package"))
            return json::error("zpm: missing 'package'");
        if (!obj.count("cwd"))
            return json::error("zpm: missing 'cwd'");

        std::string pkg = obj["package"];
        std::string cwd = obj["cwd"];

        std::string shared_path = "/shared/" + pkg;
        if (!db.exists(shared_path))
            return "\033[31mError:\033[0m Package '" + pkg + "' not found in global registry.";

        std::string content = db.read_file(shared_path);

        std::string dest_path = cwd;
        if (dest_path.back() != '/')
            dest_path += "/";
        dest_path += pkg;

        std::string actual_dest = scope_path(session_id, dest_path);
        db.write_file(actual_dest, content);

        return "\033[32mSuccess:\033[0m Package '" + pkg + "' installed to " + dest_path;
    }

    // === List shared files ===
    if (cmd == "shared")
    {
        if (!db.exists("/shared"))
            return "(no shared files)";
        auto entries = db.list_dir("/shared");
        return format_ls(entries);
    }

    // === Chat broadcast ===
    if (cmd == "chat")
    {
        std::string raw_msg = "";
        if (obj.count("path"))
            raw_msg = obj["path"];
        else if (obj.count("msg"))
            raw_msg = obj["msg"];

        std::string username = "anonymous";
        {
            std::lock_guard<std::mutex> lock(sessions_mtx);
            auto it = session_to_user.find(session_id);
            if (it != session_to_user.end())
                username = it->second;
        }

        std::string target_user = "global";
        std::string text = raw_msg;

        if (raw_msg.rfind("@", 0) == 0)
        {
            size_t space = raw_msg.find(' ');
            if (space != std::string::npos)
            {
                target_user = raw_msg.substr(1, space - 1);
                text = raw_msg.substr(space + 1);
            }
        }

        // Escape text for safe JSON embedding
        std::string escaped_text;
        for (char c : text)
        {
            if (c == '"') escaped_text += "\\\"";
            else if (c == '\\') escaped_text += "\\\\";
            else if (c == '\n') escaped_text += "\\n";
            else if (c == '\r') escaped_text += "\\r";
            else if (c == '\t') escaped_text += "\\t";
            else escaped_text += c;
        }

        std::string payload = "__chat__{\"from\":\"" + username + "\",\"target\":\"" + target_user +
                              "\",\"msg\":\"" + escaped_text + "\",\"sid\":\"" + session_id + "\"}";

        std::vector<int> target_fds;
        {
            std::lock_guard<std::mutex> lock1(clients_mtx);
            std::lock_guard<std::mutex> lock2(sessions_mtx);
            for (auto const& [fd, s_id] : active_clients)
            {
                if (fd == client_fd)
                    continue; // sender gets echo separately
                if (target_user == "global")
                {
                    target_fds.push_back(fd);
                }
                else
                {
                    auto it2 = session_to_user.find(s_id);
                    if (it2 != session_to_user.end() && it2->second == target_user)
                    {
                        target_fds.push_back(fd);
                    }
                }
            }
        }

        for (int fd : target_fds)
        {
            ws::send_frame(fd, 0x1, payload);
        }

        // Also send back to sender so their chat UI updates
        ws::send_frame(client_fd, 0x1, payload);
        return "";
    }

    return "unknown command: " + cmd;
}

static void handle_client(int client)
{
    if (!do_handshake(client))
    {
        close(client);
        return;
    }

    ws::Frame auth_frame = ws::decode_frame(client);
    if (auth_frame.opcode != 0x1)
    {
        close(client);
        return;
    }

    auto auth_obj = json::parse(auth_frame.payload);
    std::string session_id;

    if (auth_obj.count("session"))
    {
        std::string candidate = auth_obj["session"];
        // Security: only accept session IDs that are valid hex AND already known
        if (is_valid_session_id(candidate))
        {
            std::lock_guard<std::mutex> lock(sessions_mtx);
            if (known_sessions.count(candidate))
                session_id = candidate;
        }
    }

    if (session_id.empty())
    {
        session_id = generate_session_id();
    }

    {
        std::lock_guard<std::mutex> lock(sessions_mtx);
        if (!known_sessions.count(session_id))
        {
            known_sessions[session_id] = true;
        }
    }

    ensure_session_root(session_id);

    ws::send_frame(client, 0x1, "__session__" + session_id);

    std::cerr << "[server] client connected (fd=" << client << " session=" << session_id << ")\n";

    {
        std::lock_guard<std::mutex> lock(clients_mtx);
        active_clients[client] = session_id;
    }

    while (true)
    {
        ws::Frame frame = ws::decode_frame(client);

        if (frame.opcode == 0x8 || frame.opcode == 0x0)
            break;

        if (frame.opcode == 0x1)
        {
            std::cerr << "[debug] raw frame (" << frame.payload.size() << " bytes): ["
                      << frame.payload << "]\n";
            std::string response = route_command(frame.payload, session_id, client);

            auto obj = json::parse(frame.payload);

            if (obj.count("pipe"))
            {
                std::string pipe_cmd = obj["pipe"];
                if (pipe_cmd.find("grep ") == 0)
                {
                    std::string search = pipe_cmd.substr(5);
                    std::istringstream iss(response);
                    std::string line;
                    std::string new_resp = "";
                    while (std::getline(iss, line))
                    {
                        if (line.find(search) != std::string::npos)
                        {
                            new_resp += line + "\n";
                        }
                    }
                    if (!new_resp.empty() && new_resp.back() == '\n')
                        new_resp.pop_back();
                    response = new_resp;
                }
                else if (pipe_cmd.find("head") == 0)
                {
                    int n = 10;
                    auto n_pos = pipe_cmd.find("-n ");
                    if (n_pos != std::string::npos)
                    {
                        try
                        {
                            n = std::stoi(pipe_cmd.substr(n_pos + 3));
                        }
                        catch (...)
                        {
                            n = 10;
                        }
                        if (n <= 0)
                            n = 10;
                    }
                    std::istringstream iss(response);
                    std::string line;
                    std::string new_resp = "";
                    int count = 0;
                    while (std::getline(iss, line) && count < n)
                    {
                        new_resp += line + "\n";
                        count++;
                    }
                    if (!new_resp.empty() && new_resp.back() == '\n')
                        new_resp.pop_back();
                    response = new_resp;
                }
                else if (pipe_cmd.find("tail") == 0)
                {
                    int n = 10;
                    auto n_pos = pipe_cmd.find("-n ");
                    if (n_pos != std::string::npos)
                    {
                        try
                        {
                            n = std::stoi(pipe_cmd.substr(n_pos + 3));
                        }
                        catch (...)
                        {
                            n = 10;
                        }
                        if (n <= 0)
                            n = 10;
                    }
                    std::istringstream iss(response);
                    std::string line;
                    std::vector<std::string> lines;
                    while (std::getline(iss, line))
                    {
                        lines.push_back(line);
                    }
                    std::string new_resp = "";
                    int start = (lines.size() > (size_t)n) ? (lines.size() - n) : 0;
                    for (size_t i = start; i < lines.size(); i++)
                    {
                        new_resp += lines[i] + "\n";
                    }
                    if (!new_resp.empty() && new_resp.back() == '\n')
                        new_resp.pop_back();
                    response = new_resp;
                }
                else if (pipe_cmd.find("wc") == 0)
                {
                    int lines = 0, words = 0, chars = response.size();
                    std::istringstream iss(response);
                    std::string line;
                    while (std::getline(iss, line))
                    {
                        lines++;
                        std::istringstream wss(line);
                        std::string word;
                        while (wss >> word)
                            words++;
                    }
                    if (pipe_cmd.find("-l") != std::string::npos)
                        response = "  " + std::to_string(lines);
                    else if (pipe_cmd.find("-w") != std::string::npos)
                        response = "  " + std::to_string(words);
                    else if (pipe_cmd.find("-c") != std::string::npos)
                        response = "  " + std::to_string(chars);
                    else
                        response = "  " + std::to_string(lines) + "  " + std::to_string(words) +
                                   "  " + std::to_string(chars);
                }
            }

            if (obj.count("redirect"))
            {
                std::string red_file = obj["redirect"];
                bool append = obj.count("append") ? (obj["append"] == "true") : false;
                std::string scoped_red = scope_path(session_id, red_file);

                if (append)
                {
                    std::string existing = db.exists(scoped_red) ? db.read_file(scoped_red) : "";
                    if (!existing.empty() && existing.back() != '\n')
                        existing += '\n';
                    db.write_file(scoped_red, existing + response + "\n");
                }
                else
                {
                    db.write_file(scoped_red, response + "\n");
                }
                response = ""; // Suppress output
            }

            // Always send response so client can stop waiting and refresh prompt
            ws::send_frame(client, 0x1, response);
        }

        if (frame.opcode == 0x9)
        {
            ws::send_frame(client, 0xA, frame.payload);
        }
    }

    {
        std::lock_guard<std::mutex> lock(clients_mtx);
        active_clients.erase(client);
    }

    std::cerr << "[server] client disconnected (fd=" << client << ")\n";
    close(client);
}

int main()
{
    const int port = 8080;

    std::string conninfo;
    const char* env = std::getenv("ZERORING_DB");
    if (env)
        conninfo = env;

    if (!db.connect(conninfo))
    {
        std::cerr << "[server] WARNING: database connection failed, using in-memory VFS\n";
    }
    db.init_schema();

    int listener = create_listener(port);
    if (listener < 0)
        return 1;

    std::cerr << "[server] listening on ws://localhost:" << port << "\n";

    while (true)
    {
        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client = accept(listener, (struct sockaddr*)&client_addr, &client_len);
        if (client < 0)
            continue;
        std::thread(handle_client, client).detach();
    }
}