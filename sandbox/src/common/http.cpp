#include "http.h"
#include "util.h"
#include <arpa/inet.h>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace sbx {
namespace http {

void initSignals() {
  signal(SIGPIPE, SIG_IGN);
}

namespace {

bool readLine(int fd, std::string& out, size_t limit = 64 * 1024) {
  out.clear();
  char ch;
  while (out.size() < limit) {
    ssize_t n = read(fd, &ch, 1);
    if (n <= 0) return false;
    if (ch == '\n') return true;
    if (ch != '\r') out.push_back(ch);
  }
  return false;
}

std::string urlDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) {
      auto hexVal = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
      };
      int h = hexVal(s[i + 1]), l = hexVal(s[i + 2]);
      if (h >= 0 && l >= 0) {
        out.push_back(static_cast<char>((h << 4) | l));
        i += 2;
        continue;
      }
    }
    if (s[i] == '+') out.push_back(' ');
    else out.push_back(s[i]);
  }
  return out;
}

} // namespace

std::string Request::queryParam(const std::string& key) const {
  size_t q = path.find('?');
  if (q == std::string::npos) return "";
  std::string query = path.substr(q + 1);
  for (const auto& pair : split(query, '&')) {
    size_t eq = pair.find('=');
    std::string k = eq == std::string::npos ? pair : pair.substr(0, eq);
    std::string v = eq == std::string::npos ? "" : pair.substr(eq + 1);
    if (urlDecode(k) == key) return urlDecode(v);
  }
  return "";
}

void ResponseWriter::write(const std::string& data) {
  if (ended_) return;
  if (!began_) {
    std::string head;
    if (status_ == 200) head = "HTTP/1.1 200 OK\r\n";
    else if (status_ == 400) head = "HTTP/1.1 400 Bad Request\r\n";
    else if (status_ == 401) head = "HTTP/1.1 401 Unauthorized\r\n";
    else if (status_ == 403) head = "HTTP/1.1 403 Forbidden\r\n";
    else if (status_ == 404) head = "HTTP/1.1 404 Not Found\r\n";
    else if (status_ == 429) head = "HTTP/1.1 429 Too Many Requests\r\n";
    else if (status_ == 500) head = "HTTP/1.1 500 Internal Server Error\r\n";
    else head = "HTTP/1.1 " + std::to_string(status_) + " Status\r\n";
    head += "Connection: close\r\n";
    for (const auto& kv : headers_) head += kv.first + ": " + kv.second + "\r\n";
    head += "\r\n";
    ::write(fd_, head.data(), head.size());
    began_ = true;
  }
  ::write(fd_, data.data(), data.size());
}

void ResponseWriter::end() {
  if (ended_) return;
  ended_ = true;
  if (!began_) write("");
  ::close(fd_);
}

void Server::handle(const std::string& method, const std::string& pathPrefix, Handler h) {
  routes_.push_back({method, pathPrefix, std::move(h)});
}

void Server::listen(const std::string& addr, int port) {
  fd_ = socket(AF_INET, SOCK_STREAM, 0);
  int opt = 1;
  setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  sockaddr_in saddr{};
  saddr.sin_family = AF_INET;
  saddr.sin_port = htons(static_cast<uint16_t>(port));
  if (addr.empty() || addr == "0.0.0.0") {
    saddr.sin_addr.s_addr = INADDR_ANY;
  } else if (inet_pton(AF_INET, addr.c_str(), &saddr.sin_addr) != 1) {
    close(fd_);
    fd_ = -1;
    throw std::runtime_error("invalid listen address '" + addr + "'");
  }
  if (bind(fd_, reinterpret_cast<sockaddr*>(&saddr), sizeof(saddr)) != 0) {
    close(fd_);
    fd_ = -1;
    throw std::runtime_error("bind failed on " + addr + ":" + std::to_string(port));
  }
  ::listen(fd_, 128);
}

void Server::handleConnection(int clientFd) {
  Request req;
  std::string line;
  if (!readLine(clientFd, line)) { ::close(clientFd); return; }
  auto parts = split(line, ' ');
  if (parts.size() < 3) { ::close(clientFd); return; }
  req.method = parts[0];
  req.path = parts[1];
  size_t q = req.path.find('?');
  req.pathOnly = q == std::string::npos ? req.path : req.path.substr(0, q);

  while (readLine(clientFd, line)) {
    if (line.empty()) break;
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = trim(line.substr(0, colon));
    for (auto& ch : key) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    req.headers[key] = trim(line.substr(colon + 1));
  }

  std::string cl = req.header("content-length");
  if (!cl.empty()) {
    long len = strtol(cl.c_str(), nullptr, 10);
    if (len > 0 && len < (1 << 30)) {
      std::string buf;
      buf.resize(static_cast<size_t>(len));
      size_t got = 0;
      while (got < static_cast<size_t>(len)) {
        ssize_t n = read(clientFd, &buf[got], static_cast<size_t>(len) - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
      }
      req.body.assign(buf.data(), got);
    }
  }

  ResponseWriter writer(clientFd);
  bool matched = false;
  for (const auto& route : routes_) {
    if (route.method != req.method) continue;
    bool match = req.pathOnly == route.path;
    if (!match && req.pathOnly.size() > route.path.size() &&
        req.pathOnly.compare(0, route.path.size(), route.path) == 0) {
      match = req.pathOnly[route.path.size()] == '/';
    }
    if (match) {
      matched = true;
      route.handler(req, writer);
      break;
    }
  }
  if (!matched) {
    writer.setStatus(404);
    writer.write("{\"error\":\"not found\"}");
  }
  writer.end();
}

void Server::run() {
  while (true) {
    int clientFd = accept(fd_, nullptr, nullptr);
    if (clientFd < 0) {
      if (errno == EINTR) continue;
      break;
    }
    std::thread([this, clientFd]() { handleConnection(clientFd); }).detach();
  }
}

} // namespace http
} // namespace sbx