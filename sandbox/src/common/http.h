#pragma once
#include <functional>
#include <map>
#include <string>

namespace sbx {
namespace http {

struct Request {
  std::string method;
  std::string path;
  std::string pathOnly;
  std::map<std::string, std::string> headers;
  std::string body;

  std::string header(const std::string& key) const {
    auto it = headers.find(key);
    return it == headers.end() ? "" : it->second;
  }
  std::string queryParam(const std::string& key) const;
};

class ResponseWriter {
public:
  explicit ResponseWriter(int fd) : fd_(fd) {}

  void setStatus(int code) { status_ = code; }
  void setHeader(const std::string& k, const std::string& v) { headers_[k] = v; }
  void write(const std::string& data);
  void end();
  int status() const { return status_; }

private:
  int fd_;
  int status_ = 200;
  std::map<std::string, std::string> headers_;
  bool began_ = false;
  bool ended_ = false;
};

using Handler = std::function<void(const Request&, ResponseWriter&)>;

class Server {
public:
  void handle(const std::string& method, const std::string& pathPrefix, Handler h);
  void listen(const std::string& addr, int port);
  void run();

private:
  struct Route {
    std::string method;
    std::string path;
    Handler handler;
  };
  std::vector<Route> routes_;
  int fd_ = -1;
  void handleConnection(int clientFd);
};

void initSignals();

} // namespace http
} // namespace sbx