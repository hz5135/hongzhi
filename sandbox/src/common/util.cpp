#include "util.h"
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace sbx {

namespace {
long nowMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec * 1000L + tv.tv_usec / 1000L;
}
}

std::string randomHex(size_t n) {
  const char* hex = "0123456789abcdef";
  std::string out;
  out.reserve(n);
  std::ifstream ur("/dev/urandom", std::ios::binary);
  char byte;
  for (size_t i = 0; i < n; i++) {
    ur.read(&byte, 1);
    out.push_back(hex[static_cast<unsigned char>(byte) & 0x0f]);
  }
  return out;
}

std::string trim(const std::string& s) {
  size_t begin = 0, end = s.size();
  while (begin < end && (s[begin] == ' ' || s[begin] == '\t' || s[begin] == '\r' || s[begin] == '\n')) begin++;
  while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) end--;
  return s.substr(begin, end - begin);
}

std::vector<std::string> split(const std::string& s, char delim) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (ch == delim) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  out.push_back(cur);
  return out;
}

std::string shellEscape(const std::string& s) {
  if (!s.empty() && s.find_first_of(" \t\n'\"\\$;&|<>()*?[]~`!#") == std::string::npos) {
    return s;
  }
  std::string out = "'";
  for (char ch : s) {
    if (ch == '\'') out += "'\\''";
    else out.push_back(ch);
  }
  out += "'";
  return out;
}

bool fileExists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

bool dirExists(const std::string& path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void mkdirs(const std::string& path) {
  if (path.empty()) return;
  std::string cur;
  for (char ch : path) {
    cur.push_back(ch);
    if (ch == '/' && cur.size() > 1) ::mkdir(cur.c_str(), 0755);
  }
  ::mkdir(path.c_str(), 0755);
}

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void writeFile(const std::string& path, const std::string& data) {
  std::string tmp = path + ".tmp";
  std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  f.close();
  ::rename(tmp.c_str(), path.c_str());
}

int runCapture(const std::vector<std::string>& args, std::string& out, std::string& err,
               int timeoutMs) {
  int outPipe[2], errPipe[2];
  if (pipe(outPipe) != 0 || pipe(errPipe) != 0) return -1;
  pid_t pid = fork();
  if (pid == 0) {
    dup2(outPipe[1], 1);
    dup2(errPipe[1], 2);
    close(outPipe[0]); close(errPipe[0]);
    close(outPipe[1]); close(errPipe[1]);
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  close(outPipe[1]); close(errPipe[1]);

  bool outOpen = true, errOpen = true;
  bool timedOut = false;
  long startMs = nowMs();
  char buf[4096];

  while (outOpen || errOpen) {
    struct pollfd fds[2];
    int nfds = 0;
    if (outOpen) { fds[nfds].fd = outPipe[0]; fds[nfds].events = POLLIN; nfds++; }
    if (errOpen) { fds[nfds].fd = errPipe[0]; fds[nfds].events = POLLIN; nfds++; }

    int pollTimeout = -1;
    if (timeoutMs > 0) {
      long remaining = timeoutMs - (nowMs() - startMs);
      if (remaining <= 0) { timedOut = true; break; }
      pollTimeout = static_cast<int>(remaining);
    }

    int pr = poll(fds, nfds, pollTimeout);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) { timedOut = true; break; }

    for (int i = 0; i < nfds; i++) {
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
      int fd = fds[i].fd;
      bool isOut = (fd == outPipe[0]);
      ssize_t n = read(fd, buf, sizeof(buf));
      if (n > 0) {
        if (isOut) out.append(buf, n);
        else err.append(buf, n);
      }
      if (n <= 0) {
        close(fd);
        if (isOut) outOpen = false;
        else errOpen = false;
      }
    }
  }

  if (outOpen) { close(outPipe[0]); }
  if (errOpen) { close(errPipe[0]); }

  int status = 0;
  if (timedOut) {
    out.append(" [runCapture timed out after " + std::to_string(timeoutMs) + "ms]");
    kill(pid, SIGKILL);
  }
  waitpid(pid, &status, 0);
  if (timedOut) return -2;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
}

std::string homeDir() {
  const char* h = getenv("SBX_HOME");
  if (h && *h) return h;
  const char* hh = getenv("HOME");
  return hh ? std::string(hh) + "/.sbx" : "/root/.sbx";
}

} // namespace sbx