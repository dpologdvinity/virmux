#include "virmux/session.hpp"

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace virmux {

namespace {

/**
 * @brief Replace user home directory in path with ~ to prevent path leaking.
 */
std::string collapse_home_path(const std::string& str) {
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == '\0' || std::string_view(home) == "/") {
    return str;
  }
  std::string home_str(home);
  if (str.find(home_str) == std::string::npos) {
    return str;
  }
  std::string res = str;
  size_t pos = 0;
  while ((pos = res.find(home_str, pos)) != std::string::npos) {
    res.replace(pos, home_str.length(), "~");
    pos += 1;
  }
  return res;
}

/**
 * @brief Escape characters for JSON string serialization.
 */
std::string escape_json(std::string_view input) {
  std::string output;
  output.reserve(input.size() + 8);
  for (char c : input) {
    switch (c) {
      case '"':
        output += "\\\"";
        break;
      case '\\':
        output += "\\\\";
        break;
      case '\b':
        output += "\\b";
        break;
      case '\f':
        output += "\\f";
        break;
      case '\n':
        output += "\\n";
        break;
      case '\r':
        output += "\\r";
        break;
      case '\t':
        output += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(
              buf, sizeof(buf), "\\u%04x",
              static_cast<unsigned int>(static_cast<unsigned char>(c)));
          output += buf;
        } else {
          output += c;
        }
        break;
    }
  }
  return output;
}

/**
 * @brief Derive a default session name from command binary basename.
 */
std::string derive_session_name(const std::string& name, const std::string& cmd,
                                uint64_t fallback_id) {
  if (!name.empty()) {
    return name;
  }
  std::string_view sv = cmd;
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' ||
                         sv.front() == '\r' || sv.front() == '\n')) {
    sv.remove_prefix(1);
  }
  if (!sv.empty()) {
    auto end = sv.find_first_of(" \t\r\n");
    std::string_view token =
        (end == std::string_view::npos) ? sv : sv.substr(0, end);
    std::string filename = std::filesystem::path(token).filename().string();
    if (!filename.empty() && filename != "." && filename != "/") {
      return filename;
    }
  }
  return "session-" + std::to_string(fallback_id);
}

/**
 * @brief Recursively terminate a process and all its children.
 */
void kill_process_tree(pid_t pid, int signal, const std::string& procfs_root) {
  if (pid <= 0) return;

  // 1. Recurse over children from procfs
  const std::filesystem::path children_path =
      std::filesystem::path(procfs_root) / std::to_string(pid) / "task" /
      std::to_string(pid) / "children";
  std::ifstream children_file(children_path);
  if (children_file.is_open()) {
    pid_t child_pid = 0;
    while (children_file >> child_pid) {
      if (child_pid > 0 && child_pid != pid) {
        kill_process_tree(child_pid, signal, procfs_root);
      }
    }
  }

  // 2. Signal target process
  ::kill(pid, signal);

  // 3. Signal process group if pid > 1
  if (pid > 1) {
    ::kill(-pid, signal);
  }
}

}  // namespace

std::string_view session_state_to_string(SessionState s) {
  switch (s) {
    case SessionState::INITIALIZING:
      return "INITIALIZING";
    case SessionState::RUNNING_BURST:
      return "RUNNING_BURST";
    case SessionState::RUNNING_NORMAL:
      return "RUNNING_NORMAL";
    case SessionState::WAITING_IO:
      return "WAITING_IO";
    case SessionState::FROZEN:
      return "FROZEN";
    case SessionState::DEAD:
      return "DEAD";
  }
  return "UNKNOWN";
}

SessionState session_state_from_string(std::string_view str) {
  if (str == "INITIALIZING") {
    return SessionState::INITIALIZING;
  }
  if (str == "RUNNING_BURST") {
    return SessionState::RUNNING_BURST;
  }
  if (str == "RUNNING_NORMAL") {
    return SessionState::RUNNING_NORMAL;
  }
  if (str == "WAITING_IO") {
    return SessionState::WAITING_IO;
  }
  if (str == "FROZEN") {
    return SessionState::FROZEN;
  }
  if (str == "DEAD") {
    return SessionState::DEAD;
  }
  return SessionState::INITIALIZING;
}

SessionRegistry::SessionRegistry(std::string procfs_root)
    : procfs_root_(std::move(procfs_root)), next_id_(1) {}

SessionRegistry::~SessionRegistry() = default;

uint64_t SessionRegistry::register_session(const SessionRegistration& req) {
  std::lock_guard<std::mutex> lock(mutex_);
  SessionInfo info;
  info.id = next_id_++;
  info.pid = req.pid;
  info.name = derive_session_name(req.name, req.command, info.id);
  info.state = (req.pid > 0) ? SessionState::RUNNING_NORMAL : SessionState::DEAD;
  info.virtual_cores = (req.virtual_cores > 0) ? req.virtual_cores : 2;
  info.memory_limit_bytes = req.memory_limit_bytes;
  info.memory_rss_bytes = 0;
  info.ksm_dedup_bytes = 0;
  info.cgroup_path = collapse_home_path(req.cgroup_path);
  info.command = collapse_home_path(req.command);
  info.start_time = std::chrono::system_clock::now();
  info.last_cpu_ticks = 0;
  info.last_active_time = std::chrono::steady_clock::now();

  sessions_.push_back(info);
  return info.id;
}

bool SessionRegistry::unregister_session(uint64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  dead_since_.erase(id);
  auto it = std::find_if(sessions_.begin(), sessions_.end(),
                         [id](const SessionInfo& s) { return s.id == id; });
  if (it != sessions_.end()) {
    sessions_.erase(it);
    return true;
  }
  return false;
}

std::optional<SessionInfo> SessionRegistry::get_session(uint64_t id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(sessions_.begin(), sessions_.end(),
                         [id](const SessionInfo& s) { return s.id == id; });
  if (it != sessions_.end()) {
    return *it;
  }
  return std::nullopt;
}

std::optional<SessionInfo> SessionRegistry::get_session_by_pid(
    pid_t pid) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(sessions_.begin(), sessions_.end(),
                         [pid](const SessionInfo& s) { return s.pid == pid; });
  if (it != sessions_.end()) {
    return *it;
  }
  return std::nullopt;
}

std::vector<SessionInfo> SessionRegistry::list_sessions() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sessions_;
}

void SessionRegistry::set_auto_freeze(bool enabled, uint64_t idle_timeout_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto_freeze_enabled_ = enabled;
  idle_timeout_ms_ = idle_timeout_ms;
}

bool SessionRegistry::auto_freeze_enabled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return auto_freeze_enabled_;
}

uint64_t SessionRegistry::idle_timeout_ms() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return idle_timeout_ms_;
}

void SessionRegistry::set_on_auto_freeze(
    std::function<void(uint64_t id, pid_t pid)> cb) {
  std::lock_guard<std::mutex> lock(mutex_);
  on_auto_freeze_ = std::move(cb);
}

void SessionRegistry::refresh_telemetry() {
  std::vector<std::pair<uint64_t, pid_t>> newly_frozen;
  std::function<void(uint64_t, pid_t)> freeze_cb;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    freeze_cb = on_auto_freeze_;
    auto now_steady = std::chrono::steady_clock::now();

    long page_sz = ::sysconf(_SC_PAGESIZE);
    if (page_sz <= 0) {
      page_sz = 4096;
    }

    for (auto& s : sessions_) {
      if (s.pid <= 0) {
        s.state = SessionState::DEAD;
        s.memory_rss_bytes = 0;
        s.ksm_dedup_bytes = 0;
        if (dead_since_.find(s.id) == dead_since_.end()) {
          dead_since_[s.id] = now_steady;
        }
        continue;
      }

      // 1. Process liveness check
      if (procfs_root_ == "/proc") {
        int kill_res = ::kill(s.pid, 0);
        if (kill_res != 0 && errno == ESRCH) {
          s.state = SessionState::DEAD;
          s.memory_rss_bytes = 0;
          s.ksm_dedup_bytes = 0;
          if (dead_since_.find(s.id) == dead_since_.end()) {
            dead_since_[s.id] = now_steady;
          }
          continue;
        }
      }

      const std::filesystem::path proc_pid =
          std::filesystem::path(procfs_root_) / std::to_string(s.pid);

      // 2. Check proc status for zombie or stopped states and fallback RSS
      bool is_zombie = false;
      bool is_stopped = false;
      bool status_opened = false;
      uint64_t vmrss_bytes = 0;

      const std::filesystem::path status_path = proc_pid / "status";
      std::ifstream status_file(status_path);
      if (status_file.is_open()) {
        status_opened = true;
        std::string line;
        while (std::getline(status_file, line)) {
          if (line.starts_with("State:")) {
            size_t idx = 6;
            while (idx < line.size() &&
                   std::isspace(static_cast<unsigned char>(line[idx]))) {
              idx++;
            }
            if (idx < line.size()) {
              char st = line[idx];
              if (st == 'Z') {
                is_zombie = true;
              } else if (st == 'T') {
                is_stopped = true;
              }
            }
          } else if (line.starts_with("VmRSS:")) {
            auto colon = line.find(':');
            if (colon != std::string::npos) {
              uint64_t val_kb = 0;
              std::stringstream ss(line.substr(colon + 1));
              if (ss >> val_kb) {
                vmrss_bytes = val_kb * 1024ULL;
              }
            }
          }
        }
      }

      if (is_zombie) {
        s.state = SessionState::DEAD;
        s.memory_rss_bytes = 0;
        s.ksm_dedup_bytes = 0;
        if (dead_since_.find(s.id) == dead_since_.end()) {
          dead_since_[s.id] = now_steady;
        }
        continue;
      }

      // 3. Read RSS from statm
      bool got_rss = false;
      const std::filesystem::path statm_path = proc_pid / "statm";
      std::ifstream statm(statm_path);
      if (statm.is_open()) {
        uint64_t total_pages = 0;
        uint64_t rss_pages = 0;
        if (statm >> total_pages >> rss_pages) {
          s.memory_rss_bytes = rss_pages * static_cast<uint64_t>(page_sz);
          got_rss = true;
        }
      }

      if (!got_rss && vmrss_bytes > 0) {
        s.memory_rss_bytes = vmrss_bytes;
        got_rss = true;
      }

      // 4. Read KSM dedup
      s.ksm_dedup_bytes = 0;
      const std::filesystem::path ksm_pages_path = proc_pid / "ksm_merging_pages";
      std::ifstream ksm_file(ksm_pages_path);
      if (ksm_file.is_open()) {
        uint64_t ksm_pages = 0;
        if (ksm_file >> ksm_pages) {
          s.ksm_dedup_bytes = ksm_pages * static_cast<uint64_t>(page_sz);
        }
      } else {
        const std::filesystem::path smaps_path = proc_pid / "smaps_rollup";
        std::ifstream smaps_file(smaps_path);
        if (smaps_file.is_open()) {
          std::string line;
          while (std::getline(smaps_file, line)) {
            if (line.starts_with("KSM:")) {
              auto colon = line.find(':');
              if (colon != std::string::npos) {
                uint64_t kb = 0;
                std::stringstream ss(line.substr(colon + 1));
                if (ss >> kb) {
                  s.ksm_dedup_bytes = kb * 1024ULL;
                }
              }
              break;
            }
          }
        }
      }

      // 5. Update freeze state from external signals / cgroup
      if (s.state != SessionState::DEAD) {
        bool is_frozen = is_stopped;
        bool cgroup_freeze_opened = false;
        if (!s.cgroup_path.empty()) {
          const std::filesystem::path freeze_file =
              std::filesystem::path(s.cgroup_path) / "cgroup.freeze";
          std::ifstream ff(freeze_file);
          if (ff.is_open()) {
            cgroup_freeze_opened = true;
            int val = 0;
            if (ff >> val && val == 1) {
              is_frozen = true;
            }
          }
        }

        if (is_frozen) {
          s.state = SessionState::FROZEN;
        } else if (s.state == SessionState::FROZEN &&
                   (status_opened || cgroup_freeze_opened)) {
          s.state = SessionState::RUNNING_NORMAL;
          s.last_active_time = now_steady;
        }
      }

      // 6. CPU ticks & Inactivity tracking for auto-freeze
      if (s.state != SessionState::DEAD) {
        const std::filesystem::path stat_path = proc_pid / "stat";
        std::ifstream stat_file(stat_path);
        if (stat_file.is_open()) {
          std::string stat_content;
          if (std::getline(stat_file, stat_content)) {
            auto paren = stat_content.rfind(')');
            if (paren != std::string::npos && paren + 1 < stat_content.size()) {
              std::string_view rest(stat_content.data() + paren + 1,
                                     stat_content.size() - (paren + 1));
              uint64_t utime = 0;
              uint64_t stime = 0;
              size_t token_idx = 0;
              size_t i = 0;
              while (i < rest.size()) {
                while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) {
                  i++;
                }
                if (i >= rest.size()) break;
                size_t tok_start = i;
                while (i < rest.size() && rest[i] != ' ' && rest[i] != '\t') {
                  i++;
                }
                if (token_idx == 11) {
                  std::from_chars(rest.data() + tok_start, rest.data() + i,
                                  utime);
                } else if (token_idx == 12) {
                  std::from_chars(rest.data() + tok_start, rest.data() + i,
                                  stime);
                  break;
                }
                token_idx++;
              }

              uint64_t total_ticks = utime + stime;
              if (total_ticks > s.last_cpu_ticks) {
                s.last_cpu_ticks = total_ticks;
                s.last_active_time = now_steady;
                if (s.state == SessionState::FROZEN) {
                  s.state = SessionState::RUNNING_NORMAL;
                }
              } else if (total_ticks == s.last_cpu_ticks &&
                         auto_freeze_enabled_) {
                if (s.state == SessionState::RUNNING_NORMAL ||
                    s.state == SessionState::WAITING_IO) {
                  auto elapsed_ms =
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          now_steady - s.last_active_time)
                          .count();
                  if (elapsed_ms >= static_cast<int64_t>(idle_timeout_ms_)) {
                    bool frozen = false;
                    if (!s.cgroup_path.empty()) {
                      const std::filesystem::path freeze_file =
                          std::filesystem::path(s.cgroup_path) / "cgroup.freeze";
                      std::error_code ec;
                      if (std::filesystem::exists(freeze_file, ec)) {
                        std::ofstream ofs(freeze_file);
                        if (ofs.is_open()) {
                          ofs << "1\n";
                          ofs.flush();
                          if (!ofs.fail()) {
                            frozen = true;
                          }
                        }
                      }
                    }

                    if (!frozen && s.pid > 0) {
                      if (::kill(s.pid, SIGSTOP) == 0) {
                        frozen = true;
                      } else if (procfs_root_ != "/proc") {
                        frozen = true;
                      }
                    }

                    s.state = SessionState::FROZEN;
                    newly_frozen.emplace_back(s.id, s.pid);
                  }
                }
              }
            }
          }
        }
      }
    }

    // Automatically prune DEAD sessions that exceeded 5s retention
    sessions_.erase(
        std::remove_if(
            sessions_.begin(), sessions_.end(),
            [this, now_steady](const SessionInfo& s) {
              if (s.state != SessionState::DEAD) {
                dead_since_.erase(s.id);
                return false;
              }
              auto it = dead_since_.find(s.id);
              if (it == dead_since_.end()) {
                dead_since_[s.id] = now_steady;
                return false;
              }
              auto elapsed =
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      now_steady - it->second);
              if (elapsed >= std::chrono::milliseconds(5000)) {
                dead_since_.erase(it);
                return true;
              }
              return false;
            }),
        sessions_.end());
  }

  // Invoke compaction callback outside mutex
  if (freeze_cb) {
    for (const auto& [id, pid] : newly_frozen) {
      freeze_cb(id, pid);
    }
  }
}

size_t SessionRegistry::prune_dead_sessions(
    std::chrono::milliseconds max_age) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto now = std::chrono::steady_clock::now();
  size_t count_before = sessions_.size();

  sessions_.erase(
      std::remove_if(
          sessions_.begin(), sessions_.end(),
          [this, now, max_age](const SessionInfo& s) {
            if (s.state != SessionState::DEAD) {
              dead_since_.erase(s.id);
              return false;
            }
            auto it = dead_since_.find(s.id);
            if (it == dead_since_.end()) {
              dead_since_[s.id] = now;
              it = dead_since_.find(s.id);
            }
            if (max_age.count() <= 0) {
              dead_since_.erase(it);
              return true;
            }
            auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - it->second);
            if (elapsed >= max_age) {
              dead_since_.erase(it);
              return true;
            }
            return false;
          }),
      sessions_.end());

  return count_before - sessions_.size();
}

bool SessionRegistry::freeze_session(uint64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(sessions_.begin(), sessions_.end(),
                         [id](const SessionInfo& s) { return s.id == id; });
  if (it == sessions_.end()) {
    return false;
  }
  if (it->state == SessionState::DEAD || it->pid <= 0) {
    return false;
  }

  bool frozen = false;
  if (!it->cgroup_path.empty()) {
    const std::filesystem::path freeze_file =
        std::filesystem::path(it->cgroup_path) / "cgroup.freeze";
    std::error_code ec;
    if (std::filesystem::exists(freeze_file, ec)) {
      std::ofstream ofs(freeze_file);
      if (ofs.is_open()) {
        ofs << "1\n";
        ofs.flush();
        if (!ofs.fail()) {
          frozen = true;
        }
      }
    }
  }

  if (!frozen) {
    if (::kill(it->pid, SIGSTOP) == 0) {
      frozen = true;
    } else {
      if (procfs_root_ != "/proc") {
        frozen = true;
      } else {
        if (errno == ESRCH) {
          it->state = SessionState::DEAD;
          it->memory_rss_bytes = 0;
        }
        return false;
      }
    }
  }

  it->state = SessionState::FROZEN;
  return true;
}

bool SessionRegistry::thaw_session(uint64_t id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(sessions_.begin(), sessions_.end(),
                         [id](const SessionInfo& s) { return s.id == id; });
  if (it == sessions_.end()) {
    return false;
  }
  if (it->state == SessionState::DEAD || it->pid <= 0) {
    return false;
  }

  bool thawed = false;
  if (!it->cgroup_path.empty()) {
    const std::filesystem::path freeze_file =
        std::filesystem::path(it->cgroup_path) / "cgroup.freeze";
    std::error_code ec;
    if (std::filesystem::exists(freeze_file, ec)) {
      std::ofstream ofs(freeze_file);
      if (ofs.is_open()) {
        ofs << "0\n";
        ofs.flush();
        if (!ofs.fail()) {
          thawed = true;
        }
      }
    }
  }

  if (!thawed) {
    if (::kill(it->pid, SIGCONT) == 0) {
      thawed = true;
    } else {
      if (procfs_root_ != "/proc") {
        thawed = true;
      } else {
        if (errno == ESRCH) {
          it->state = SessionState::DEAD;
          it->memory_rss_bytes = 0;
        }
        return false;
      }
    }
  }

  it->state = SessionState::RUNNING_NORMAL;
  it->last_active_time = std::chrono::steady_clock::now();
  return true;
}

bool SessionRegistry::kill_session(uint64_t id, int signal) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = std::find_if(sessions_.begin(), sessions_.end(),
                         [id](const SessionInfo& s) { return s.id == id; });
  if (it == sessions_.end()) {
    return false;
  }
  if (it->state == SessionState::DEAD) {
    return true;
  }
  if (it->pid <= 0) {
    it->state = SessionState::DEAD;
    it->memory_rss_bytes = 0;
    if (dead_since_.find(it->id) == dead_since_.end()) {
      dead_since_[it->id] = std::chrono::steady_clock::now();
    }
    return true;
  }

  // 1. Terminate entire cgroup subtree atomically if cgroup.kill exists
  if (!it->cgroup_path.empty()) {
    const std::filesystem::path cg_kill =
        std::filesystem::path(it->cgroup_path) / "cgroup.kill";
    std::error_code ec;
    if (std::filesystem::exists(cg_kill, ec)) {
      std::ofstream ofs(cg_kill);
      if (ofs.is_open()) {
        ofs << "1\n";
        ofs.flush();
      }
    }
  }

  // 2. Terminate target process and all child processes
  kill_process_tree(it->pid, signal, procfs_root_);

  // 3. Mark session dead if killed or departed
  if (signal == SIGKILL || (::kill(it->pid, 0) == -1 && errno == ESRCH)) {
    it->state = SessionState::DEAD;
    it->memory_rss_bytes = 0;
    if (dead_since_.find(it->id) == dead_since_.end()) {
      dead_since_[it->id] = std::chrono::steady_clock::now();
    }
  }

  return true;
}

std::string SessionRegistry::to_json() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (sessions_.empty()) {
    return "[]";
  }

  std::ostringstream oss;
  oss << "[\n";
  for (size_t i = 0; i < sessions_.size(); ++i) {
    const auto& s = sessions_[i];
    auto start_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                             s.start_time.time_since_epoch())
                             .count();
    oss << "  {\n"
        << "    \"id\": " << s.id << ",\n"
        << "    \"name\": \"" << escape_json(s.name) << "\",\n"
        << "    \"pid\": " << s.pid << ",\n"
        << "    \"state\": \"" << session_state_to_string(s.state) << "\",\n"
        << "    \"virtual_cores\": " << s.virtual_cores << ",\n"
        << "    \"memory_rss_bytes\": " << s.memory_rss_bytes << ",\n"
        << "    \"memory_limit_bytes\": " << s.memory_limit_bytes << ",\n"
        << "    \"ksm_dedup_bytes\": " << s.ksm_dedup_bytes << ",\n"
        << "    \"cgroup_path\": \"" << escape_json(s.cgroup_path) << "\",\n"
        << "    \"command\": \"" << escape_json(s.command) << "\",\n"
        << "    \"start_time\": " << start_time_ms << "\n"
        << "  }" << (i + 1 < sessions_.size() ? ",\n" : "\n");
  }
  oss << "]";
  return oss.str();
}

std::vector<SessionInfo> SessionRegistry::from_json(std::string_view json) {
  std::vector<SessionInfo> results;
  size_t idx = 0;

  auto skip_ws = [&]() {
    while (idx < json.size() && (json[idx] == ' ' || json[idx] == '\t' ||
                                 json[idx] == '\r' || json[idx] == '\n')) {
      idx++;
    }
  };

  skip_ws();
  if (idx >= json.size() || json[idx] != '[') {
    return results;
  }
  idx++;  // skip '['

  while (idx < json.size()) {
    skip_ws();
    if (idx < json.size() && json[idx] == ']') {
      idx++;
      break;
    }
    if (json[idx] == ',') {
      idx++;
      continue;
    }
    if (json[idx] != '{') {
      idx++;
      continue;
    }
    idx++;  // skip '{'

    SessionInfo info;
    uint64_t start_time_ms = 0;

    while (idx < json.size()) {
      skip_ws();
      if (idx < json.size() && json[idx] == '}') {
        idx++;
        break;
      }
      if (json[idx] == ',') {
        idx++;
        continue;
      }

      if (json[idx] != '"') {
        idx++;
        continue;
      }
      idx++;  // skip opening '"'
      size_t key_start = idx;
      while (idx < json.size() && json[idx] != '"') {
        idx++;
      }
      std::string_view key = json.substr(key_start, idx - key_start);
      if (idx < json.size()) {
        idx++;  // skip closing '"'
      }

      skip_ws();
      if (idx < json.size() && json[idx] == ':') {
        idx++;
      }
      skip_ws();

      if (idx < json.size() && json[idx] == '"') {
        idx++;  // skip opening '"'
        std::string val_str;
        while (idx < json.size() && json[idx] != '"') {
          if (json[idx] == '\\' && idx + 1 < json.size()) {
            char next = json[idx + 1];
            if (next == '"') {
              val_str += '"';
              idx += 2;
              continue;
            } else if (next == '\\') {
              val_str += '\\';
              idx += 2;
              continue;
            } else if (next == 'n') {
              val_str += '\n';
              idx += 2;
              continue;
            } else if (next == 'r') {
              val_str += '\r';
              idx += 2;
              continue;
            } else if (next == 't') {
              val_str += '\t';
              idx += 2;
              continue;
            }
          }
          val_str += json[idx];
          idx++;
        }
        if (idx < json.size()) {
          idx++;  // skip closing '"'
        }

        if (key == "name") {
          info.name = val_str;
        } else if (key == "state") {
          info.state = session_state_from_string(val_str);
        } else if (key == "cgroup_path") {
          info.cgroup_path = val_str;
        } else if (key == "command") {
          info.command = val_str;
        }
      } else {
        size_t val_start = idx;
        while (idx < json.size() &&
               (std::isdigit(static_cast<unsigned char>(json[idx])) ||
                json[idx] == '-')) {
          idx++;
        }
        std::string_view num_sv = json.substr(val_start, idx - val_start);
        int64_t val_num = 0;
        if (!num_sv.empty()) {
          std::from_chars(num_sv.data(), num_sv.data() + num_sv.size(),
                          val_num);
        }

        if (key == "id") {
          info.id = static_cast<uint64_t>(val_num);
        } else if (key == "pid") {
          info.pid = static_cast<pid_t>(val_num);
        } else if (key == "virtual_cores") {
          info.virtual_cores = static_cast<int>(val_num);
        } else if (key == "memory_rss_bytes") {
          info.memory_rss_bytes = static_cast<uint64_t>(val_num);
        } else if (key == "memory_limit_bytes") {
          info.memory_limit_bytes = static_cast<uint64_t>(val_num);
        } else if (key == "ksm_dedup_bytes") {
          info.ksm_dedup_bytes = static_cast<uint64_t>(val_num);
        } else if (key == "start_time" || key == "start_time_ms") {
          start_time_ms = static_cast<uint64_t>(val_num);
        }
      }
    }

    if (start_time_ms > 0) {
      info.start_time = std::chrono::system_clock::time_point(
          std::chrono::milliseconds(start_time_ms));
    }
    results.push_back(info);
  }

  return results;
}

}  // namespace virmux
