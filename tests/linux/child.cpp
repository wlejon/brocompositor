#include "linux/child.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

extern char** environ;

namespace bctest {

std::string which(const std::string& program) {
    if (program.find('/') != std::string::npos) return access(program.c_str(), X_OK) == 0 ? program : "";
    const char* path = std::getenv("PATH");
    std::stringstream ss(path ? path : "/usr/bin:/bin");
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        std::string full = dir + "/" + program;
        if (access(full.c_str(), X_OK) == 0) return full;
    }
    return {};
}

std::unique_ptr<Child> Child::spawn(const std::vector<std::string>& argv, const std::vector<std::string>& env,
                                    bool capture_stderr) {
    if (argv.empty()) return nullptr;
    std::string exe = which(argv[0]);
    if (exe.empty()) return nullptr;

    std::map<std::string, std::string> vars;
    for (char** e = environ; *e; ++e) {
        std::string s = *e;
        auto eq = s.find('=');
        if (eq != std::string::npos) vars[s.substr(0, eq)] = s.substr(eq + 1);
    }
    for (const auto& s : env) {
        auto eq = s.find('=');
        if (eq == std::string::npos) vars.erase(s);  // "K" alone unsets
        else vars[s.substr(0, eq)] = s.substr(eq + 1);
    }
    std::vector<std::string> envs;
    for (auto& [k, v] : vars) envs.push_back(k + "=" + v);
    std::vector<char*> envp, args;
    for (auto& s : envs) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<std::string> argv_copy = argv;
    for (auto& s : argv_copy) args.push_back(s.data());
    args.push_back(nullptr);

    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC) != 0) return nullptr;
    if (pipe2(out, O_CLOEXEC) != 0) {
        ::close(in[0]);
        ::close(in[1]);
        return nullptr;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    if (capture_stderr) posix_spawn_file_actions_adddup2(&fa, out[1], 2);
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    // Own process group, so kill_now() also reaches grandchildren (foot's shell).
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid = -1;
    int rc = posix_spawn(&pid, exe.c_str(), &fa, &attr, args.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&attr);
    ::close(in[0]);
    ::close(out[1]);
    if (rc != 0) {
        ::close(in[1]);
        ::close(out[0]);
        return nullptr;
    }
    auto c = std::unique_ptr<Child>(new Child());
    c->pid_ = pid;
    c->stdin_fd_ = in[1];
    c->stdout_fd_ = out[0];
    Child* raw = c.get();
    c->reader_ = std::thread([raw] {
        std::string partial;
        char buf[4096];
        for (;;) {
            ssize_t n = read(raw->stdout_fd_, buf, sizeof buf);
            if (n <= 0) break;
            partial.append(buf, size_t(n));
            size_t nl;
            while ((nl = partial.find('\n')) != std::string::npos) {
                std::lock_guard<std::mutex> lock(raw->m_);
                raw->lines_.push_back(partial.substr(0, nl));
                partial.erase(0, nl + 1);
                raw->cv_.notify_all();
            }
        }
        std::lock_guard<std::mutex> lock(raw->m_);
        if (!partial.empty()) raw->lines_.push_back(partial);
        raw->cv_.notify_all();
    });
    return c;
}

Child::~Child() {
    kill_now();
    close_stdin();
    if (reader_.joinable()) reader_.join();
    if (stdout_fd_ >= 0) ::close(stdout_fd_);
}

bool Child::wait_line(const std::string& prefix, int timeout_ms, std::string* line) {
    std::unique_lock<std::mutex> lock(m_);
    size_t seen = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        for (; seen < lines_.size(); ++seen)
            if (lines_[seen].rfind(prefix, 0) == 0) {
                if (line) *line = lines_[seen];
                return true;
            }
        if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
            for (; seen < lines_.size(); ++seen)
                if (lines_[seen].rfind(prefix, 0) == 0) {
                    if (line) *line = lines_[seen];
                    return true;
                }
            return false;
        }
    }
}

size_t Child::count(const std::string& prefix) const {
    std::lock_guard<std::mutex> lock(m_);
    size_t n = 0;
    for (auto& l : lines_) n += l.rfind(prefix, 0) == 0;
    return n;
}

bool Child::wait_count(const std::string& prefix, size_t n, int timeout_ms) {
    std::unique_lock<std::mutex> lock(m_);
    auto counted = [&] {
        size_t c = 0;
        for (auto& l : lines_) c += l.rfind(prefix, 0) == 0;
        return c >= n;
    };
    return cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), counted);
}

std::vector<std::string> Child::lines() const {
    std::lock_guard<std::mutex> lock(m_);
    return lines_;
}

std::string Child::output() const {
    std::lock_guard<std::mutex> lock(m_);
    std::string s;
    for (auto& l : lines_) s += l + "\n";
    return s;
}

void Child::send(const std::string& text) {
    if (stdin_fd_ < 0) return;
    ssize_t r = write(stdin_fd_, text.data(), text.size());
    (void)r;
}

void Child::close_stdin() {
    if (stdin_fd_ >= 0) ::close(stdin_fd_);
    stdin_fd_ = -1;
}

bool Child::wait_exit(int timeout_ms, int* status) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        if (exited_) break;
        int st = 0;
        pid_t r = waitpid(pid_, &st, WNOHANG);
        if (r == pid_) {
            exited_ = true;
            status_ = WIFEXITED(st) ? WEXITSTATUS(st) : -WTERMSIG(st);
            break;
        }
        if (r < 0) {
            exited_ = true;
            status_ = -1;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        poll(nullptr, 0, 10);
    }
    if (status) *status = status_;
    return true;
}

bool Child::running() { return !wait_exit(0); }

void Child::kill_now() {
    if (pid_ > 0 && !exited_) {
        ::kill(-pid_, SIGKILL);
        ::kill(pid_, SIGKILL);
        wait_exit(5000);
    }
}

void Child::terminate(int timeout_ms) {
    if (pid_ > 0 && !exited_) {
        ::kill(pid_, SIGTERM);
        if (wait_exit(timeout_ms)) return;
    }
    kill_now();
}

}  // namespace bctest
