#pragma once
#include <string>
#include <vector>

namespace sbx {

std::string randomHex(size_t n);
std::string trim(const std::string& s);
std::vector<std::string> split(const std::string& s, char delim);
std::string shellEscape(const std::string& s);
bool fileExists(const std::string& path);
bool dirExists(const std::string& path);
void mkdirs(const std::string& path);
std::string readFile(const std::string& path);
void writeFile(const std::string& path, const std::string& data);
std::string resolveHost(const std::string& hostOrIp);
std::string homeDir();

int runCapture(const std::vector<std::string>& args, std::string& out, std::string& err,
               int timeoutMs = 0);

} // namespace sbx