#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// 本机私密配置只从环境或 .env 读取，不进入公共 VGDS 数据接口。
#include <windows.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <cctype>

namespace veylaxis
{
inline void loadRuntimeEnvironment()
{
    static std::once_flag once;
    std::call_once(once,
                   []
                   {
                       namespace fs = std::filesystem;
                       std::vector<fs::path> paths;
                       if (auto explicitPath = std::getenv("VEYLAXIS_ENV_FILE"))
                           paths.push_back(fs::u8path(explicitPath));
                       else
                       {
                           paths.push_back(fs::current_path() / ".env");
                           wchar_t executable[32768]{};
                           if (GetModuleFileNameW(nullptr, executable, 32768))
                           {
                               auto dir = fs::path(executable).parent_path();
                               for (int i = 0; i < 4 && !dir.empty(); ++i, dir = dir.parent_path())
                                   paths.push_back(dir / ".env");
                           }
                       }
                       for (const auto& path : paths)
                       {
                           std::ifstream input(path);
                           if (!input)
                               continue;
                           std::string line;
                           auto trim = [](std::string s)
                           {
                               auto begin = s.find_first_not_of(" \t\r\n");
                               if (begin == std::string::npos)
                                   return std::string{};
                               return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
                           };
                           while (std::getline(input, line))
                           {
                               line = trim(line);
                               if (line.empty() || line[0] == '#')
                                   continue;
                               auto separator = line.find('=');
                               if (separator == std::string::npos)
                                   continue;
                               auto key = trim(line.substr(0, separator));
                               auto value = trim(line.substr(separator + 1));
                               if (key.empty() ||
                                   !(std::isalpha(static_cast<unsigned char>(key[0])) || key[0] == '_'))
                                   continue;
                               bool valid = true;
                               for (unsigned char c : key)
                                   if (!std::isalnum(c) && c != '_')
                                       valid = false;
                               if (!valid || std::getenv(key.c_str()))
                                   continue;
                               if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                                                         (value.front() == '\'' && value.back() == '\'')))
                                   value = value.substr(1, value.size() - 2);
                               _putenv_s(key.c_str(), value.c_str());
                               SetEnvironmentVariableA(key.c_str(), value.c_str());
                           }
                           break;
                       }
                   });
}
}
