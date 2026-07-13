#include <iostream>
#include <map>
#include <string>

std::map<std::string, std::string> parse(const std::string& input)
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

int main() {
    std::string test = "{\"cmd\":\"echo\",\"path\":\"print('Appended!')\",\"redirect\":\"/projects/script.py\",\"append\":\"true\"}";
    auto obj = parse(test);
    std::cout << "Parsed " << obj.size() << " elements\n";
    for (auto& [k, v] : obj) {
        std::cout << k << " = " << v << "\n";
    }
    return 0;
}
