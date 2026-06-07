#pragma once
#include <string>
#include <map>
#include <vector>

struct DBManager {
    std::map<std::string, std::string> store;
    void connect() {}
};
