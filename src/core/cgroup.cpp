/**
 * @file cgroup.cpp
 * @brief Implementation of cgroups v2 slice controller with unprivileged
 * fallback.
 */

#include "virmux/cgroup.hpp"

#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>

#include "virmux/raii.hpp"

namespace virmux {

namespace {

/** @brief True if @p dir's cgroup.procs and cgroup.subtree_control are both
 * writable, i.e. we may create children and migrate processes there. */
bool cgroup_writable(const std::string& dir) {
  return ::access((dir + "/cgroup.procs").c_str(), W_OK) == 0 &&
         ::access((dir + "/cgroup.subtree_control").c_str(), W_OK) == 0;
}

/** @brief Extract the path of the cgroup v2 ("0::") line, if any. */
std::optional<std::string> unified_cgroup_path(const std::string& text) {
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    if (line.starts_with("0::")) {
      std::string rel = line.substr(3);
      // A cgroup removed under us is reported with a " (deleted)" suffix.
      constexpr std::string_view kDeleted = " (deleted)";
      if (rel.ends_with(kDeleted)) {
        rel.resize(rel.size() - kDeleted.size());
      }
      return rel;
    }
  }
  return std::nullopt;
}

}  // namespace

CgroupProbe CgroupProbe::current() {
  CgroupProbe probe;
  std::ifstream in("/proc/self/cgroup");
  std::ostringstream text;
  text << in.rdbuf();
  probe.proc_self_cgroup = text.str();
  probe.uid = ::getuid();
  return probe;
}

CgroupDelegation find_cgroup_delegation(const CgroupProbe& probe) {
  CgroupDelegation d;
  const auto rel = unified_cgroup_path(probe.proc_self_cgroup);
  std::error_code ec;
  if (!rel || !rel->starts_with("/") ||
      !std::filesystem::exists(probe.root + "/cgroup.controllers", ec)) {
    d.reason = "cgroup v2 (unified hierarchy) is not mounted at " + probe.root;
    return d;
  }

  // Ancestors from the cgroup root down to our own cgroup.
  std::vector<std::string> chain{probe.root};
  std::string own = probe.root;
  for (const auto& part : std::filesystem::path(*rel).relative_path()) {
    own += "/" + part.string();
    chain.push_back(own);
  }
  d.self = own;

  // 1. The systemd user manager's subtree, nearest first.
  const std::string user_mgr = "user@" + std::to_string(probe.uid) + ".service";
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    if (std::filesystem::path(*it).filename() == user_mgr &&
        cgroup_writable(*it)) {
      d.kind = DelegationKind::kUserManager;
      d.root = *it;
      return d;
    }
  }

  // 2. Otherwise the highest writable ancestor (root, containers, CI).
  for (const auto& dir : chain) {
    if (cgroup_writable(dir)) {
      d.kind = DelegationKind::kWritableAncestor;
      d.root = dir;
      return d;
    }
  }

  if (own == probe.root) {
    // Containers and systems without systemd (e.g. WSL without systemd):
    // there is no user manager to point at.
    d.reason =
        "this process sits at the cgroup root, which it cannot write, "
        "and no delegated cgroup subtree is available";
    return d;
  }
  d.reason = "this shell's cgroup (" +
             std::filesystem::path(own).filename().string() +
             ") is outside the systemd user manager; run virmux under "
             "`systemd-run --user --scope virmux run ...` to get limits";
  return d;
}

std::string describe_limit_failures(const std::vector<LimitResult>& results,
                                    bool pinned) {
  std::string failed;
  for (const auto& r : results) {
    const bool covered_elsewhere = r.file == "cpuset.cpus" ||
                                   r.file == "memory.swap.max" ||
                                   (pinned && r.file == "cpu.max");
    if (r.applied || (covered_elsewhere && r.error == ENOENT)) {
      continue;
    }
    failed += failed.empty() ? "" : ", ";
    failed += r.file + " (" + std::strerror(r.error) + ")";
  }
  return failed.empty() ? failed : "some limits were not applied: " + failed;
}

std::optional<bool> process_in_cgroup(pid_t pid, const std::string& cgroup_dir,
                                      const std::string& cgroup_root) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/cgroup");
  if (!in) {
    return std::nullopt;
  }
  std::ostringstream text;
  text << in.rdbuf();
  const auto rel = unified_cgroup_path(text.str());
  if (!rel) {
    return std::nullopt;
  }
  const std::string where = *rel == "/" ? cgroup_root : cgroup_root + *rel;
  return where == cgroup_dir;
}

CgroupSlice::CgroupSlice(std::string name) : name_(std::move(name)) {}

CgroupSlice::~CgroupSlice() {
  if (created_ && !path_.empty()) {
    // cgroupfs directories are removed with rmdir even though they list
    // control files. EBUSY (processes outlived us) is expected and tolerated;
    // sweep_stale_runs() retries later.
    (void)::rmdir(path_.c_str());
  }
}

CgroupSlice::CgroupSlice(CgroupSlice&& other) noexcept
    : name_(std::move(other.name_)),
      path_(std::move(other.path_)),
      valid_(other.valid_),
      created_(other.created_) {
  other.valid_ = false;
  other.created_ = false;
}

CgroupSlice& CgroupSlice::operator=(CgroupSlice&& other) noexcept {
  if (this != &other) {
    if (created_ && !path_.empty()) {
      (void)::rmdir(path_.c_str());
    }
    name_ = std::move(other.name_);
    path_ = std::move(other.path_);
    valid_ = other.valid_;
    created_ = other.created_;
    other.valid_ = false;
    other.created_ = false;
  }
  return *this;
}

bool CgroupSlice::init(const std::string& slice_name) {
  if (!slice_name.empty()) {
    name_ = slice_name;
  }
  return setup("/sys/fs/cgroup/virmux.slice", 0, "");
}

bool CgroupSlice::setup(const std::string& base_cgroup_path,
                        uint64_t memory_high_bytes,
                        const std::string& cpuset_cpus_str) {
  if (base_cgroup_path.empty()) {
    if (!name_.empty()) {
      path_ = "/sys/fs/cgroup/" + name_;
    } else {
      path_ = "/sys/fs/cgroup/virmux.slice";
    }
  } else if (name_.empty()) {
    path_ = base_cgroup_path;
  } else {
    // The base path already names this slice if it *is* the slice name or
    // ends with "/<name>"; otherwise the slice is created beneath it.
    const std::string suffix = "/" + name_;
    const bool already_named =
        base_cgroup_path == name_ || base_cgroup_path.ends_with(suffix);
    if (already_named) {
      path_ = base_cgroup_path;
    } else {
      std::filesystem::path p(base_cgroup_path);
      p /= name_;
      path_ = p.string();
    }
  }

  std::error_code ec;
  bool exists = std::filesystem::exists(path_, ec);
  if (ec) {
    last_errno_ = ec.value();
    valid_ = false;
    return false;
  }

  if (!exists) {
    std::filesystem::create_directories(path_, ec);
    if (ec) {
      last_errno_ = ec.value();
      valid_ = false;
      return false;
    }
    created_ = true;
  }

  // A cgroup is usable once processes can join it; limits are best-effort and
  // independent of each other (a non-delegated cpuset must not cost us the
  // memory limit).
  valid_ = ::access((std::filesystem::path(path_) / "cgroup.procs").c_str(),
                    W_OK) == 0;
  if (!valid_) {
    last_errno_ = errno;
    return false;
  }

  CgroupLimits limits;
  if (memory_high_bytes > 0) {
    limits.memory_high = memory_high_bytes;
  }
  limits.cpuset_cpus = cpuset_cpus_str;
  (void)apply_limits(limits);
  return true;
}

bool CgroupSlice::write_file(const std::string& filename,
                             const std::string& value) {
  if (path_.empty()) {
    return false;
  }

  std::filesystem::path p(path_);
  p /= filename;
  std::string full_path = p.string();

  // No O_CREAT: on cgroupfs a missing file means the controller is not
  // enabled here, and on any other filesystem creating it would only hide that.
  int fd = ::open(full_path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) {
    last_errno_ = errno;
    return false;
  }

  ScopedFd scoped(fd);
  ssize_t written = ::write(scoped.get(), value.data(), value.size());
  if (written < 0 || static_cast<size_t>(written) != value.size()) {
    last_errno_ = written < 0 ? errno : EIO;
    return false;
  }

  last_errno_ = 0;
  return true;
}

std::vector<LimitResult> CgroupSlice::apply_limits(const CgroupLimits& limits) {
  std::vector<LimitResult> results;
  auto apply = [&](const char* file, std::string value) {
    LimitResult r{.file = file, .value = std::move(value)};
    if (path_.empty()) {
      r.error = ENOENT;
    } else {
      r.applied = write_file(r.file, r.value + "\n");
      r.error = r.applied ? 0 : last_errno_;
    }
    results.push_back(std::move(r));
  };
  if (limits.memory_max) {
    apply("memory.max", std::to_string(*limits.memory_max));
  }
  if (limits.memory_high) {
    apply("memory.high", std::to_string(*limits.memory_high));
  }
  if (limits.swap_max) {
    apply("memory.swap.max", std::to_string(*limits.swap_max));
  }
  if (!limits.cpu_max.empty()) {
    apply("cpu.max", limits.cpu_max);
  }
  if (!limits.cpuset_cpus.empty()) {
    apply("cpuset.cpus", limits.cpuset_cpus);
  }
  if (limits.pids_max) {
    apply("pids.max", *limits.pids_max == 0 ? std::string("max")
                                            : std::to_string(*limits.pids_max));
  }
  return results;
}

bool CgroupSlice::set_memory_max(uint64_t bytes) {
  return !path_.empty() &&
         write_file("memory.max", std::to_string(bytes) + "\n");
}

bool CgroupSlice::set_swap_max(uint64_t bytes) {
  return !path_.empty() &&
         write_file("memory.swap.max", std::to_string(bytes) + "\n");
}

bool CgroupSlice::set_cpu_max(const std::string& value) {
  return !path_.empty() && write_file("cpu.max", value + "\n");
}

bool CgroupSlice::set_memory_high(uint64_t bytes) {
  if (path_.empty()) {
    return false;
  }
  std::string val = std::to_string(bytes) + "\n";
  if (!write_file("memory.high", val)) {
    return false;
  }
  return true;
}

bool CgroupSlice::set_cpuset(const std::string& cores_str) {
  if (path_.empty()) {
    return false;
  }
  std::string val = cores_str + "\n";
  if (!write_file("cpuset.cpus", val)) {
    return false;
  }
  return true;
}

bool CgroupSlice::set_pids_max(uint64_t max_pids) {
  if (path_.empty()) {
    return false;
  }
  std::string val =
      (max_pids == 0) ? "max\n" : (std::to_string(max_pids) + "\n");
  if (!write_file("pids.max", val)) {
    return false;
  }
  return true;
}

bool CgroupSlice::attach_pid(pid_t pid) {
  if (!valid_ || path_.empty()) {
    return false;
  }
  std::string val = std::to_string(pid) + "\n";
  return write_file("cgroup.procs", val);
}

bool CgroupSlice::freeze() {
  if (!valid_ || path_.empty()) {
    return false;
  }
  return write_file("cgroup.freeze", "1\n");
}

bool CgroupSlice::thaw() {
  if (!valid_ || path_.empty()) {
    return false;
  }
  return write_file("cgroup.freeze", "0\n");
}

const std::string& CgroupSlice::path() const noexcept { return path_; }

const std::string& CgroupSlice::name() const noexcept { return name_; }

bool CgroupSlice::is_valid() const noexcept { return valid_; }

bool CgroupSlice::valid() const noexcept { return valid_; }

int CgroupSlice::last_error() const noexcept { return last_errno_; }

std::vector<std::string> controllers_to_enable(const std::string& available,
                                               const std::string& enabled) {
  static constexpr std::string_view kWanted[] = {"cpu", "cpuset", "memory",
                                                 "pids"};
  std::vector<std::string> on;
  std::istringstream enabled_in(enabled);
  for (std::string c; enabled_in >> c;) {
    on.push_back(c);
  }
  std::vector<std::string> todo;
  std::istringstream available_in(available);
  for (std::string c; available_in >> c;) {
    const bool wanted = std::find(std::begin(kWanted), std::end(kWanted), c) !=
                        std::end(kWanted);
    if (wanted && std::find(on.begin(), on.end(), c) == on.end()) {
      todo.push_back(c);
    }
  }
  return todo;
}

CgroupSlice create_run(const CgroupDelegation& delegation,
                       const std::string& name) {
  CgroupSlice slice(name);
  if (!delegation.usable() || delegation.root.empty() || name.empty()) {
    return slice;
  }
  const std::string base = delegation.root + "/" + kVirmuxCgroupDir;
  if (::mkdir(base.c_str(), 0755) != 0 && errno != EEXIST) {
    slice.last_errno_ = errno;
    return slice;
  }

  // Hand every controller D offers down to the runs. Each is enabled on its
  // own so one refusal (say, cpuset) does not block the others.
  auto slurp = [](const std::string& file) {
    std::ifstream in(file);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
  };
  const std::string control = base + "/cgroup.subtree_control";
  for (const auto& c : controllers_to_enable(
           slurp(base + "/cgroup.controllers"), slurp(control))) {
    const int fd = ::open(control.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
      ScopedFd scoped(fd);
      const std::string req = "+" + c;
      // A refused controller is not fatal: its control files will be
      // missing and apply_limits() reports the affected limits.
      if (::write(scoped.get(), req.data(), req.size()) < 0) {
        continue;
      }
    }
  }

  (void)slice.setup(base);
  return slice;
}

size_t sweep_stale_runs(const std::string& virmux_dir) {
  size_t removed = 0;
  std::error_code ec;
  for (const auto& entry :
       std::filesystem::directory_iterator(virmux_dir, ec)) {
    const std::string fname = entry.path().filename().string();
    if (!fname.starts_with("run-")) {
      continue;
    }
    // run-<pid>-<seq>: the pid of the virmux process that created it.
    const char* digits = fname.c_str() + 4;
    char* end = nullptr;
    errno = 0;
    const long pid = std::strtol(digits, &end, 10);
    if (end == digits || *end != '-' || errno != 0 || pid <= 0) {
      continue;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH) {
      continue;  // creator alive (or not ours to judge)
    }
    if (::rmdir(entry.path().c_str()) == 0) {
      ++removed;
    }
  }
  return removed;
}

}  // namespace virmux
