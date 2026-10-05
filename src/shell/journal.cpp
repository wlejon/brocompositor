#include "shell/journal.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

namespace brocompositor::shell {

namespace fs = std::filesystem;

namespace {

constexpr const char* kMagic = "brocompositor-journal 1";
constexpr const char* kSuffix = ".journal";

// <pid>-<start> or <pid>-<start>-<instance>.
bool parse_name(const std::string& stem, uint32_t* pid, uint64_t* start) {
    auto dash = stem.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= stem.size()) return false;
    try {
        size_t used = 0;
        unsigned long long p = std::stoull(stem.substr(0, dash), &used);
        if (used != dash) return false;
        std::string rest = stem.substr(dash + 1);
        auto dash2 = rest.find('-');
        std::string start_s = rest.substr(0, dash2);
        unsigned long long s = std::stoull(start_s, &used);
        if (used != start_s.size() || start_s.empty() || !std::isdigit(static_cast<unsigned char>(start_s[0])))
            return false;
        if (dash2 != std::string::npos) {
            std::string inst = rest.substr(dash2 + 1);
            if (inst.empty() || !std::isdigit(static_cast<unsigned char>(inst[0]))) return false;
            if (std::stoull(inst, &used) == 0 || used != inst.size()) return false;  // instances count from 2
        }
        *pid = uint32_t(p);
        *start = uint64_t(s);
        return true;
    } catch (...) {
        return false;
    }
}

// Journal files of the instances alive in this process.
std::mutex g_live_mutex;
std::set<fs::path> g_live;

}  // namespace

Journal::Journal(fs::path dir, uint32_t pid, uint64_t pid_start) : dir_(std::move(dir)) {
    if (dir_.empty()) return;
    std::string base = std::to_string(pid) + "-" + std::to_string(pid_start);
    std::lock_guard<std::mutex> lock(g_live_mutex);
    for (int n = 1;; ++n) {
        fs::path f = dir_ / (base + (n == 1 ? std::string() : "-" + std::to_string(n)) + kSuffix);
        if (g_live.insert(f).second) {
            file_ = f;
            break;
        }
    }
}

Journal::~Journal() {
    if (file_.empty()) return;
    std::lock_guard<std::mutex> lock(g_live_mutex);
    g_live.erase(file_);
}

std::string Journal::serialize(const JournalState& s) {
    std::ostringstream o;
    o << kMagic << "\n";
    for (const ParkedEntry& p : s.parked) {
        o << "park " << p.window << " " << p.pid << " " << p.pid_start << " " << p.restore.x << " " << p.restore.y
          << " " << p.restore.width << " " << p.restore.height << " " << p.parked.x << " " << p.parked.y << " "
          << p.parked.width << " " << p.parked.height << " " << p.method << "\n";
    }
    for (uint64_t r : s.reservations) o << "reservation " << r << "\n";
    o << "end\n";
    return o.str();
}

bool Journal::parse(const std::string& text, JournalState* out) {
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != kMagic) return false;
    JournalState s;
    bool ended = false;
    while (std::getline(in, line)) {
        std::istringstream l(line);
        std::string kind;
        l >> kind;
        if (kind == "park") {
            ParkedEntry p;
            l >> p.window >> p.pid >> p.pid_start >> p.restore.x >> p.restore.y >> p.restore.width >>
                p.restore.height >> p.parked.x >> p.parked.y >> p.parked.width >> p.parked.height >> p.method;
            if (l.fail()) return false;
            s.parked.push_back(p);
        } else if (kind == "reservation") {
            uint64_t r = 0;
            l >> r;
            if (l.fail()) return false;
            s.reservations.push_back(r);
        } else if (kind == "end") {
            ended = true;
            break;
        } else if (!kind.empty()) {
            return false;
        }
    }
    // A torn write (no "end") is rejected; writes are rename-atomic, so this
    // only guards against foreign files.
    if (!ended) return false;
    *out = std::move(s);
    return true;
}

bool Journal::write(const JournalState& state) {
    if (file_.empty()) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    if (state.empty()) {
        fs::remove(file_, ec);
        return !ec;
    }
    fs::create_directories(dir_, ec);
    fs::path tmp = file_;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << serialize(state);
        f.flush();
        if (!f) return false;
    }
    fs::rename(tmp, file_, ec);  // atomic replace on NTFS and POSIX
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

void Journal::remove() {
    if (file_.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    std::error_code ec;
    fs::remove(file_, ec);
}

std::vector<StaleJournal> Journal::claim_stale(const fs::path& dir,
                                               const std::function<bool(uint32_t, uint64_t)>& alive,
                                               uint32_t self_pid) {
    std::vector<StaleJournal> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec)) return out;
    std::vector<fs::path> candidates;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& p = it->path();
        std::string name = p.filename().string();
        if (p.extension() == kSuffix) {
            candidates.push_back(p);
            continue;
        }
        // A journal claimed by an instance that died while recovering it.
        auto mark = name.find(std::string(kSuffix) + ".recovering-");
        if (mark == std::string::npos) continue;
        try {
            uint32_t recoverer = uint32_t(std::stoul(name.substr(mark + std::string(kSuffix).size() + 12)));
            if (recoverer != self_pid && !alive(recoverer, 0)) candidates.push_back(p);
        } catch (...) {
        }
    }
    for (const fs::path& p : candidates) {
        uint32_t pid = 0;
        uint64_t start = 0;
        std::string name = p.filename().string();
        std::string stem = name.substr(0, name.find(kSuffix));
        if (!parse_name(stem, &pid, &start)) continue;
        if (p.extension() == kSuffix && alive(pid, start)) continue;
        // Claim: only one starting instance wins the rename.
        fs::path claimed = dir / (stem + kSuffix + ".recovering-" + std::to_string(self_pid));
        fs::rename(p, claimed, ec);
        if (ec) continue;
        StaleJournal j;
        j.file = claimed;
        j.name = stem + kSuffix;
        j.pid = pid;
        j.pid_start = start;
        std::ifstream f(claimed, std::ios::binary);
        std::stringstream text;
        text << f.rdbuf();
        if (!parse(text.str(), &j.state)) j.state = JournalState{};
        out.push_back(std::move(j));
    }
    return out;
}

void Journal::discard(const StaleJournal& j) {
    std::error_code ec;
    fs::remove(j.file, ec);
}

}  // namespace brocompositor::shell
