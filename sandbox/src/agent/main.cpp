#include "../common/http.h"
#include "../common/json.h"
#include "../common/util.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <map>
#include <pwd.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

using sbx::json::Value;
using sbx::http::Request;
using sbx::http::ResponseWriter;

namespace {

const char* kToken = "";
/* Time from guest kernel boot (== firecracker instance start) to the init
   process, in seconds; -1 if unavailable. Reported on /health. */
double g_bootToInitSec = -1;
double g_initMonotonicSec = -1;
double g_agentStartMonotonicSec = -1;
double g_agentStartUnixSec = -1;
int g_activeCommands = 0;
std::mutex g_activeMu;
std::mutex g_cmdMu;
std::map<std::string, pid_t> g_cmdPids;
std::map<std::string, int> g_cmdPtys;

std::string now() {
  char buf[64];
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tmv;
  gmtime_r(&tv.tv_sec, &tmv);
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
  std::string s = buf;
  char ms[16];
  snprintf(ms, sizeof(ms), ".%06ldZ", static_cast<long>(tv.tv_usec));
  return s + ms;
}

std::string eventJson(const char* type, const Value& extra) {
  Value v = Value::object();
  v.set("type", Value::text(type));
  for (const auto& kv : extra.obj) v.set(kv.first, kv.second);
  v.set("ts", Value::text(now()));
  return sbx::json::stringify(v) + "\n";
}

bool authorized(const Request& req) {
  std::string auth = req.header("authorization");
  const std::string prefix = "Bearer ";
  if (auth.compare(0, prefix.size(), prefix) != 0) return false;
  return auth.substr(prefix.size()) == kToken;
}

// Read the per-VM agent token passed by the host on the guest kernel command
// line (`agentbench.token=<t>`). This lets the injected rootfs be a fully
// static template reused by every VM — no per-VM file needs writing.
//
// The token ends at the first whitespace, not at the first space: /proc/cmdline
// is newline-terminated, so when the token is the last argument on the line
// there is no space to stop at and the newline becomes part of the token. That
// is the case for cloud-hypervisor, which takes the command line verbatim,
// while firecracker appends its own `root=` / `virtio_mmio.device=` arguments
// after ours and so always left a space. A token with a newline in it can never
// match an Authorization header, and the failure is silent at create time:
// /health is the one route without an auth check, so the agent still reports
// healthy and only exec and file transfer come back 401.
std::string readTokenFromCmdline() {
  FILE* f = fopen("/proc/cmdline", "r");
  if (!f) return "";
  char buf[8192];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  if (n == 0) return "";
  buf[n] = '\0';
  const char* p = strstr(buf, "agentbench.token=");
  if (!p) return "";
  p += std::strlen("agentbench.token=");
  return std::string(p, strcspn(p, " \t\r\n"));
}

double monotonicSeconds() {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

double unixSeconds() {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0) return -1;
  return static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
}

double readInitMonotonicSec() {
  FILE* f = fopen("/var/log/sbx-init-monotonic", "r");
  if (!f) return -1;
  double value = -1;
  if (fscanf(f, "%lf", &value) != 1) value = -1;
  fclose(f);
  unlink("/var/log/sbx-init-monotonic");
  return value;
}

// Time from kernel boot to the init process, parsed from the kernel ring
// buffer. The kernel logs "Run <path> as init process" as it execs init; the
// record's microsecond timestamp is the instance-start -> init latency, i.e.
// the true firecracker startup (no host setup, no userspace).
double kernelBootToInitSec() {
  int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
  if (fd < 0) return -1;
  std::string data;
  char buf[8192];
  ssize_t n;
  while ((n = read(fd, buf, sizeof(buf))) > 0) data.append(buf, static_cast<size_t>(n));
  close(fd);
  // Records: "<pri>,<seq>,<usec>,<flags>[,...];<text>"; usec is field 2.
  size_t pos = 0;
  while (pos < data.size()) {
    size_t nl = data.find('\n', pos);
    if (nl == std::string::npos) nl = data.size();
    std::string line = data.substr(pos, nl - pos);
    pos = nl + 1;
    if (line.find("as init process") == std::string::npos) continue;
    size_t c1 = line.find(',');
    size_t c2 = (c1 == std::string::npos) ? std::string::npos : line.find(',', c1 + 1);
    size_t c3 = (c2 == std::string::npos) ? std::string::npos : line.find(',', c2 + 1);
    if (c3 == std::string::npos) continue;
    return atof(line.c_str() + c2 + 1) / 1e6;
  }
  return -1;
}

bool tryReserveCommand() {
  std::lock_guard<std::mutex> lk(g_activeMu);
  if (g_activeCommands >= 16) return false;
  g_activeCommands++;
  return true;
}
void releaseCommand() {
  std::lock_guard<std::mutex> lk(g_activeMu);
  g_activeCommands--;
}

int uidForUser(const std::string& user) {
  if (user.empty() || user == "root") return 0;
  struct passwd* pw = getpwnam(user.c_str());
  return pw ? pw->pw_uid : -1;
}

struct PipeReader {
  int fd;
  const char* type;
  pid_t pid;
};

void handleExec(const Request& req, ResponseWriter& w) {
  Value reqBody;
  try {
    reqBody = sbx::json::parse(req.body);
  } catch (...) {
    w.setStatus(400);
    w.write("{\"error\":\"invalid request body\"}");
    return;
  }
  std::string cmd = reqBody.get("cmd").strOr("");
  if (cmd.empty()) {
    w.setStatus(400);
    w.write("{\"error\":\"cmd is required\"}");
    return;
  }
  if (!tryReserveCommand()) {
    w.setStatus(429);
    w.write("{\"error\":\"too many concurrent commands\"}");
    return;
  }

  Value argsArr = reqBody.get("args");
  std::vector<std::string> args;
  args.push_back(cmd);
  if (argsArr.isArray()) {
    for (size_t i = 0; i < argsArr.size(); i++) {
      args.push_back(argsArr.at(i).strOr(""));
    }
  }

  std::string cwd = reqBody.get("cwd").strOr("");
  std::string user = reqBody.get("user").strOr("");
  long timeoutMs = reqBody.get("timeoutMs").intOr(0);
  bool tty = reqBody.get("tty").boolOr(false);
  Value envObj = reqBody.get("env");

  int uid = uidForUser(user);
  if (uid < 0) {
    releaseCommand();
    w.setStatus(400);
    w.write("{\"error\":\"resolve user\"}");
    return;
  }

  int masterFd = -1, slaveFd = -1;
  int outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1};
  if (tty) {
    if (openpty(&masterFd, &slaveFd, nullptr, nullptr, nullptr) != 0) {
      releaseCommand();
      w.setStatus(500);
      w.write("{\"error\":\"openpty\"}");
      return;
    }
  } else if (pipe(outPipe) != 0 || pipe(errPipe) != 0) {
    if (outPipe[0] >= 0) close(outPipe[0]);
    if (outPipe[1] >= 0) close(outPipe[1]);
    if (errPipe[0] >= 0) close(errPipe[0]);
    if (errPipe[1] >= 0) close(errPipe[1]);
    releaseCommand();
    w.setStatus(500);
    w.write("{\"error\":\"pipe\"}");
    return;
  }

  pid_t pid = fork();
  if (pid < 0) {
    if (masterFd >= 0) close(masterFd);
    if (slaveFd >= 0) close(slaveFd);
    if (outPipe[0] >= 0) close(outPipe[0]);
    if (outPipe[1] >= 0) close(outPipe[1]);
    if (errPipe[0] >= 0) close(errPipe[0]);
    if (errPipe[1] >= 0) close(errPipe[1]);
    releaseCommand();
    w.setStatus(500);
    w.write("{\"error\":\"fork\"}");
    return;
  }

  if (pid == 0) {
    if (tty) {
      setsid();
      if (ioctl(slaveFd, TIOCSCTTY, 0) != 0) _exit(126);
      dup2(slaveFd, 0);
      dup2(slaveFd, 1);
      dup2(slaveFd, 2);
      if (slaveFd > 2) close(slaveFd);
      close(masterFd);
    } else {
      setpgid(0, 0);
      dup2(outPipe[1], 1);
      dup2(errPipe[1], 2);
      close(outPipe[0]); close(errPipe[0]);
      close(outPipe[1]); close(errPipe[1]);
    }
    if (uid != 0) {
      struct passwd* pw = getpwnam(user.c_str());
      if (pw) {
        setgid(pw->pw_gid);
        setuid(pw->pw_uid);
      }
    }
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
    if (envObj.isObject()) {
      for (const auto& kv : envObj.obj) {
        setenv(kv.first.c_str(), kv.second.strOr("").c_str(), 1);
      }
    }
    std::vector<char*> argv;
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    if (cmd.find('/') != std::string::npos) {
      execv(cmd.c_str(), argv.data());
    } else {
      execvp(cmd.c_str(), argv.data());
    }
    _exit(127);
  }

  if (slaveFd >= 0) close(slaveFd);
  close(outPipe[1]); close(errPipe[1]);

  std::string cmdId = sbx::randomHex(16);
  {
    std::lock_guard<std::mutex> lk(g_cmdMu);
    g_cmdPids[cmdId] = pid;
    if (masterFd >= 0) g_cmdPtys[cmdId] = masterFd;
  }

  w.setStatus(200);
  w.setHeader("Content-Type", "application/x-ndjson");
  Value started = Value::object();
  started.set("id", Value::text(cmdId));
  started.set("pid", Value::number(pid));
  w.write(eventJson("started", started));

  PipeReader readers[2] = {{outPipe[0], "stdout", pid}, {errPipe[0], "stderr", pid}};
  int nReaders = 2;
  if (tty) {
    readers[0] = {masterFd, "stdout", pid};
    readers[1] = {-1, "stderr", pid};
    nReaders = 1;
  }

  bool timedOut = false;
  struct timespec deadline{};
  if (timeoutMs > 0) {
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeoutMs / 1000;
    deadline.tv_nsec += (timeoutMs % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000L;
    }
  }

  int openReaders = nReaders;
  while (openReaders > 0) {
    fd_set rfds;
    FD_ZERO(&rfds);
    int maxFd = -1;
    for (const auto& r : readers) {
      if (r.fd >= 0) {
        FD_SET(r.fd, &rfds);
        if (r.fd > maxFd) maxFd = r.fd;
      }
    }
    struct timeval tv{};
    struct timeval* tvp = nullptr;
    if (timeoutMs > 0) {
      struct timespec nowTs;
      clock_gettime(CLOCK_MONOTONIC, &nowTs);
      long remainMs = (deadline.tv_sec - nowTs.tv_sec) * 1000 +
                      (deadline.tv_nsec - nowTs.tv_nsec) / 1000000;
      if (remainMs <= 0) {
        timedOut = true;
        kill(-pid, SIGKILL);
        break;
      }
      tv.tv_sec = remainMs / 1000;
      tv.tv_usec = (remainMs % 1000) * 1000;
      tvp = &tv;
    }
    int rc = select(maxFd + 1, &rfds, nullptr, nullptr, tvp);
    if (rc < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (rc == 0) {
      timedOut = true;
      kill(-pid, SIGKILL);
      break;
    }
    for (auto& r : readers) {
      if (r.fd >= 0 && FD_ISSET(r.fd, &rfds)) {
        char buf[8192];
        ssize_t n = read(r.fd, buf, sizeof(buf));
        if (n > 0) {
          Value ev = Value::object();
          ev.set("data", Value::text(std::string(buf, n)));
          w.write(eventJson(r.type, ev));
        } else {
          close(r.fd);
          r.fd = -1;
          openReaders--;
        }
      }
    }
  }

  for (auto& r : readers) {
    if (r.fd >= 0) { close(r.fd); r.fd = -1; }
  }
  if (masterFd >= 0) { close(masterFd); masterFd = -1; }

  int status = 0;
  waitpid(pid, &status, 0);
  {
    std::lock_guard<std::mutex> lk(g_cmdMu);
    g_cmdPids.erase(cmdId);
    g_cmdPtys.erase(cmdId);
  }
  releaseCommand();

  if (timedOut) {
    w.write(eventJson("timeout", Value::object()));
    return;
  }
  int code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1);
  Value exitEv = Value::object();
  exitEv.set("exitCode", Value::number(code));
  w.write(eventJson("exit", exitEv));
}

void handleKill(const Request& req, ResponseWriter& w) {
  std::string id = req.pathOnly.substr(std::string("/exec/").size());
  size_t slash = id.find('/');
  if (slash != std::string::npos) id = id.substr(0, slash);
  pid_t pid = 0;
  {
    std::lock_guard<std::mutex> lk(g_cmdMu);
    auto it = g_cmdPids.find(id);
    if (it != g_cmdPids.end()) pid = it->second;
  }
  if (pid == 0) {
    w.setStatus(404);
    w.write("{\"error\":\"command not found\"}");
    return;
  }
  kill(-pid, SIGKILL);
  w.setStatus(200);
  w.write("{\"status\":\"killed\"}");
}

void handleExecStdin(const Request& req, ResponseWriter& w) {
  std::string id = req.pathOnly.substr(std::string("/exec/").size());
  size_t slash = id.find('/');
  if (slash != std::string::npos) id = id.substr(0, slash);
  int master = -1;
  {
    std::lock_guard<std::mutex> lk(g_cmdMu);
    auto it = g_cmdPtys.find(id);
    if (it != g_cmdPtys.end()) master = it->second;
  }
  if (master < 0) {
    w.setStatus(404);
    w.write("{\"error\":\"command not found or not a tty session\"}");
    return;
  }
  const std::string& data = req.body;
  if (!data.empty()) {
    size_t off = 0;
    while (off < data.size()) {
      ssize_t n = write(master, data.data() + off, data.size() - off);
      if (n <= 0) break;
      off += static_cast<size_t>(n);
    }
  }
  w.setStatus(200);
  w.write("{\"status\":\"ok\"}");
}

void handleExecResize(const Request& req, ResponseWriter& w) {
  std::string id = req.pathOnly.substr(std::string("/exec/").size());
  size_t slash = id.find('/');
  if (slash != std::string::npos) id = id.substr(0, slash);
  int master = -1;
  {
    std::lock_guard<std::mutex> lk(g_cmdMu);
    auto it = g_cmdPtys.find(id);
    if (it != g_cmdPtys.end()) master = it->second;
  }
  if (master < 0) {
    w.setStatus(404);
    w.write("{\"error\":\"command not found or not a tty session\"}");
    return;
  }
  Value body;
  try {
    body = sbx::json::parse(req.body);
  } catch (...) {
    w.setStatus(400);
    w.write("{\"error\":\"invalid body\"}");
    return;
  }
  struct winsize ws{};
  ws.ws_row = static_cast<unsigned short>(body.get("rows").intOr(24));
  ws.ws_col = static_cast<unsigned short>(body.get("cols").intOr(80));
  ioctl(master, TIOCSWINSZ, &ws);
  w.setStatus(200);
  w.write("{\"status\":\"ok\"}");
}

void handleFilesWrite(const Request& req, ResponseWriter& w) {
  std::string extractDir = req.header("x-extract-dir");
  if (extractDir.empty()) extractDir = "/";
  if (extractDir[0] != '/') {
    w.setStatus(400);
    w.write("{\"error\":\"X-Extract-Dir must be absolute\"}");
    return;
  }
  const char* tmpDirEnv = getenv("SBX_TMP_DIR");
  std::string tmpBase = (tmpDirEnv && *tmpDirEnv) ? tmpDirEnv : "/tmp";
  std::string tmp = tmpBase + "/sbx-upload-" + sbx::randomHex(8) + ".gz";
  sbx::writeFile(tmp, req.body);
  std::string out, err;
  sbx::mkdirs(extractDir);
  int rc = sbx::runCapture({"tar", "-xzf", tmp, "-C", extractDir}, out, err);
  std::string countOut;
  if (rc == 0) sbx::runCapture({"tar", "-tzf", tmp}, countOut, err);
  std::string trimmedCount = sbx::trim(countOut);
  long filesWritten = trimmedCount.empty() ? 0 : static_cast<long>(sbx::split(trimmedCount, '\n').size());
  ::unlink(tmp.c_str());
  if (rc != 0) {
    w.setStatus(400);
    Value v = Value::object();
    v.set("error", Value::text("tar extract failed: " + err));
    w.write(sbx::json::stringify(v));
    return;
  }
  Value v = Value::object();
  v.set("filesWritten", Value::number(filesWritten));
  w.setHeader("Content-Type", "application/json");
  w.write(sbx::json::stringify(v));
}

void handleFilesRead(const Request& req, ResponseWriter& w) {
  Value body;
  try {
    body = sbx::json::parse(req.body);
  } catch (...) {
    w.setStatus(400);
    w.write("{\"error\":\"invalid request body\"}");
    return;
  }
  std::string path = body.get("path").strOr("");
  if (path.empty() || path[0] != '/') {
    w.setStatus(400);
    w.write("{\"error\":\"path must be absolute\"}");
    return;
  }
  if (!sbx::fileExists(path)) {
    w.setStatus(404);
    w.write("{\"error\":\"file not found\"}");
    return;
  }
  w.setHeader("Content-Type", "application/octet-stream");
  w.write(sbx::readFile(path));
}

/* ---- Boot-timer handshake ------------------------------------------------

   firecracker runs with `--boot-timer` for the official VM boot metric: the
   host timestamp taken at InstanceStart until the guest tells the driver that
   it is about to run init. The boot device is an MMIO pseudo-device that emits

     Guest-boot-time = <us> us <ms> ms, <cpu us> CPU us <cpu ms> CPU ms

   only for a single-byte write of 123 to offset 0 of its window (1 GiB on
   aarch64, 3 GiB on x86_64 - arch::BOOT_DEVICE_MEM_START). Anything else is
   ignored, and a guest that never writes it leaves the host with no record at
   all. Upstream signals it from a tiny init wrapper that runs just before
   exec'ing /sbin/init, which is the point of the metric: the guest's own init
   has not started yet, so no userspace boot time is included.

   The wrapper is this same binary under a second name. The injected rootfs
   installs it as /usr/local/bin/sbx-boot-init, the backend boots the guest with
   `init=/usr/local/bin/sbx-boot-init` (the kernel execs that path verbatim -
   `init=` takes no arguments), and argv[0] selects the mode. The guest template
   therefore stays the one static artifact every VM reuses.                    */
#if defined(__x86_64__)
const unsigned long kBootDeviceAddr = 0xc0000000UL; /* 4 GiB - the 1 GiB MMIO32 window */
#else
const unsigned long kBootDeviceAddr = 0x40000000UL; /* MMIO32_MEM_START */
#endif
const unsigned char kBootMagic = 123;

/* writeBootMagic signals the boot timer. A failure only costs the boot record -
   the host falls back to the guest's own kernel timestamp - so it is reported
   and never fatal. */
bool writeBootMagic(std::string& err) {
  int fd = open("/dev/mem", O_RDWR | O_SYNC);
  if (fd < 0 && errno == ENOENT) {
    /* A real root device gets devtmpfs mounted before init runs; an initramfs
       may come up with no /dev at all. */
    if (mknod("/dev/mem", S_IFCHR | 0600, makedev(1, 1)) == 0) {
      fd = open("/dev/mem", O_RDWR | O_SYNC);
    }
  }
  if (fd < 0) {
    err = std::string("open /dev/mem: ") + strerror(errno);
    return false;
  }
  void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, kBootDeviceAddr);
  if (page == MAP_FAILED) {
    err = "mmap the boot device: " + std::string(strerror(errno));
    close(fd);
    return false;
  }
  /* One byte at offset 0 is what the pseudo-device decodes. */
  *static_cast<volatile unsigned char*>(page) = kBootMagic;
  munmap(page, 4096);
  close(fd);
  return true;
}

/* Run when the kernel execs us as init: signal the boot timer, then hand the
   machine over to the real init. init keeps pid 1 and gets an almost empty
   environment, so the exec has to succeed - a program that merely returned
   would leave the kernel with no init and panic. */
int runBootInit() {
  double initMonotonic = monotonicSeconds();
  if (initMonotonic >= 0) {
    FILE* f = fopen("/var/log/sbx-init-monotonic", "w");
    if (f) {
      fprintf(f, "%.9f\n", initMonotonic);
      fclose(f);
    }
  }
  std::string err;
  if (!writeBootMagic(err)) {
    fprintf(stderr, "sbx-boot-init: %s; the VM continues without a boot record\n", err.c_str());
  }
  const char* const paths[] = {"/sbin/init", "/etc/init", "/bin/init", "/bin/sh"};
  const char* const env[] = {"HOME=/", "TERM=linux", "PATH=/sbin:/bin:/usr/sbin:/usr/bin", nullptr};
  for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
    char* const argv[] = {const_cast<char*>(paths[i]), nullptr};
    execve(paths[i], argv, const_cast<char* const*>(env));
    fprintf(stderr, "sbx-boot-init: exec %s: %s\n", paths[i], strerror(errno));
  }
  return 1;
}

/* The basename is the whole test: the kernel passes the `init=` path as argv[0],
   and the wrapper is installed under a name of its own. */
bool bootInitMode(const char* argv0) {
  const char* base = strrchr(argv0, '/');
  return strcmp(base ? base + 1 : argv0, "sbx-boot-init") == 0;
}

} // namespace

int main(int argc, char** argv) {
  /* Capture this before argument parsing, token handling and socket setup. In a
     normal container this is the container's agent-process start; in a
     microVM it is the post-init agent start. Both clocks are reported so the
     host can align the process start with Docker's StartedAt timestamp. */
  g_agentStartMonotonicSec = monotonicSeconds();
  g_agentStartUnixSec = unixSeconds();
  /* Booted as `init=/usr/local/bin/sbx-boot-init`: hand the machine to the real
     init instead of serving the agent. */
  if (bootInitMode(argv[0])) return runBootInit();
  int port = 9119;
  std::string token;
  bool tokenFromCmdline = false;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
    else if (strcmp(argv[i], "--token") == 0 && i + 1 < argc) token = argv[++i];
    else if (strcmp(argv[i], "--token-from-cmdline") == 0) tokenFromCmdline = true;
  }
  if (token.empty() && tokenFromCmdline) {
    token = readTokenFromCmdline();
  }
  if (token.empty()) {
    const char* env = getenv("SBX_AGENT_TOKEN");
    if (env) token = env;
  }
  const char* envPort = getenv("SBX_AGENT_PORT");
  if (envPort && *envPort) port = atoi(envPort);
  if (token.empty()) {
    fprintf(stderr, "auth token required: use --token, --token-from-cmdline, or SBX_AGENT_TOKEN env\n");
    return 1;
  }
  kToken = token.c_str();
  g_bootToInitSec = kernelBootToInitSec();
  g_initMonotonicSec = readInitMonotonicSec();

  sbx::http::initSignals();
  sbx::http::Server server;
  server.handle("GET", "/health", [](const Request&, ResponseWriter& w) {
    w.setHeader("Content-Type", "application/json");
    double userspace = -1;
    double userspaceStart = g_initMonotonicSec >= 0 ? g_initMonotonicSec : g_agentStartMonotonicSec;
    if (userspaceStart >= 0) {
      double now = monotonicSeconds();
      if (now >= userspaceStart) userspace = now - userspaceStart;
    }
    char b[320];
    if (g_bootToInitSec >= 0 && userspace >= 0 && g_agentStartUnixSec >= 0) {
      snprintf(b, sizeof(b), "{\"status\":\"ok\",\"boot_sec\":%.6f,\"userspace_sec\":%.6f,\"agent_start_unix_sec\":%.6f}",
               g_bootToInitSec, userspace, g_agentStartUnixSec);
    } else if (g_bootToInitSec >= 0 && userspace >= 0) {
      snprintf(b, sizeof(b), "{\"status\":\"ok\",\"boot_sec\":%.6f,\"userspace_sec\":%.6f}",
               g_bootToInitSec, userspace);
    } else if (g_bootToInitSec >= 0 && g_agentStartUnixSec >= 0) {
      snprintf(b, sizeof(b), "{\"status\":\"ok\",\"boot_sec\":%.6f,\"agent_start_unix_sec\":%.6f}",
               g_bootToInitSec, g_agentStartUnixSec);
    } else if (userspace >= 0 && g_agentStartUnixSec >= 0) {
      snprintf(b, sizeof(b), "{\"status\":\"ok\",\"userspace_sec\":%.6f,\"agent_start_unix_sec\":%.6f}",
               userspace, g_agentStartUnixSec);
    } else if (userspace >= 0) {
      snprintf(b, sizeof(b), "{\"status\":\"ok\",\"userspace_sec\":%.6f}", userspace);
    } else if (g_agentStartUnixSec >= 0) {
      snprintf(b, sizeof(b), "{\"status\":\"ok\",\"agent_start_unix_sec\":%.6f}", g_agentStartUnixSec);
    } else {
      snprintf(b, sizeof(b), "{\"status\":\"ok\"}");
    }
    w.write(b);
  });
  server.handle("POST", "/exec", [](const Request& req, ResponseWriter& w) {
    if (!authorized(req)) {
      w.setStatus(401);
      w.write("{\"error\":\"unauthorized\"}");
      return;
    }
    if (req.pathOnly.rfind("/exec/", 0) == 0) {
      if (req.pathOnly.find("/kill") != std::string::npos) { handleKill(req, w); return; }
      if (req.pathOnly.find("/stdin") != std::string::npos) { handleExecStdin(req, w); return; }
      if (req.pathOnly.find("/resize") != std::string::npos) { handleExecResize(req, w); return; }
    }
    handleExec(req, w);
  });
  server.handle("POST", "/files/write", [](const Request& req, ResponseWriter& w) {
    if (!authorized(req)) {
      w.setStatus(401);
      w.write("{\"error\":\"unauthorized\"}");
      return;
    }
    handleFilesWrite(req, w);
  });
  server.handle("POST", "/files/read", [](const Request& req, ResponseWriter& w) {
    if (!authorized(req)) {
      w.setStatus(401);
      w.write("{\"error\":\"unauthorized\"}");
      return;
    }
    handleFilesRead(req, w);
  });

  server.listen("0.0.0.0", port);
  fprintf(stderr, "sbx-agent listening on 0.0.0.0:%d\n", port);
  server.run();
  return 0;
}