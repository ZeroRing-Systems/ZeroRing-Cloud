#pragma once
#include <map>
#include <string>

namespace json
{

inline std::map<std::string, std::string> parse(const std::string& input)
{
    std::map<std::string, std::string> result;
    size_t i = 0;
    auto skip_ws = [&]()
    {
        while (i < input.size() &&
               (input[i] == ' ' || input[i] == '\t' || input[i] == '\n' || input[i] == '\r'))
            i++;
    };
    auto read_string = [&]() -> std::string
    {
        std::string s;
        if (i >= input.size() || input[i] != '"')
            return s;
        i++;
        while (i < input.size() && input[i] != '"')
        {
            if (input[i] == '\\' && i + 1 < input.size())
            {
                i++;
                if (input[i] == '"')
                    s += '"';
                else if (input[i] == '\\')
                    s += '\\';
                else if (input[i] == 'n')
                    s += '\n';
                else if (input[i] == 't')
                    s += '\t';
                else
                {
                    s += '\\';
                    s += input[i];
                }
            }
            else
            {
                s += input[i];
            }
            i++;
        }
        if (i < input.size())
            i++;
        return s;
    };

    skip_ws();
    if (i >= input.size() || input[i] != '{')
        return result;
    i++;

    while (i < input.size())
    {
        skip_ws();
        if (input[i] == '}')
            break;
        if (input[i] == ',')
        {
            i++;
            continue;
        }

        std::string key = read_string();
        if (key.empty())
            break;
        skip_ws();
        if (i < input.size() && input[i] == ':')
            i++;
        skip_ws();
        std::string val = read_string();
        if (!key.empty())
            result[key] = val;
    }
    return result;
}

inline std::string ok(const std::string& data)
{
    return "{\"status\":\"ok\",\"data\":\"" + data + "\"}";
}

inline std::string ok_raw(const std::string& data)
{
    return "{\"status\":\"ok\",\"data\":" + data + "}";
}

inline std::string error(const std::string& msg)
{
    return "{\"status\":\"error\",\"msg\":\"" + msg + "\"}";
}

inline std::string array(const std::vector<std::string>& items)
{
    std::string out = "[";
    for (size_t i = 0; i < items.size(); i++)
    {
        if (i > 0)
            out += ",";
        out += "\"" + items[i] + "\"";
    }
    out += "]";
    return out;
}

} // namespace json
