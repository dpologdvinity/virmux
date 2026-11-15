/**
 * @file cgroup.cpp
 * @brief Implementation of cgroups v2 slice controller with unprivileged fallback.
 */

#include "virmux/cgroup.hpp"
#include "virmux/raii.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <unistd.h>
#include <utility>

namespace virmux {

CgroupSlice::CgroupSlice(std::string name) : name_(std::move(name)) {}

CgroupSlice::~CgroupSlice() {
    if (created_ && !path_.empty()) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        // Error on removal (e.g. active child processes or EBUSY) is safely swallowed
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
            std::error_code ec;
            std::filesystem::remove(path_, ec);
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
        std::string suffix = "/" + name_;
        if (base_cgroup_path.size() >= suffix.size() &&
            base_cgroup_path.compare(base_cgroup_path.size() - suffix.size(), suffix.size(), suffix) == 0) {
            path_ = base_cgroup_path;
        } else if (base_cgroup_path == name_) {
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
        std::fprintf(stderr,
                     "warning: [cgroup] failed to inspect '%s': %s (running in unprivileged fallback mode)\n",
                     path_.c_str(), ec.message().c_str());
        valid_ = false;
        return false;
    }

    if (!exists) {
        std::filesystem::create_directories(path_, ec);
        if (ec) {
            std::fprintf(stderr,
                         "warning: [cgroup] failed to create cgroup directory '%s': %s (running in unprivileged fallback mode)\n",
                         path_.c_str(), ec.message().c_str());
            valid_ = false;
            return false;
        }
        created_ = true;
    }

    // Configure memory.high soft limit if requested
    if (memory_high_bytes > 0) {
        if (!set_memory_high(memory_high_bytes)) {
            valid_ = false;
            return false;
        }
    }

    // Configure cpuset.cpus if requested
    if (!cpuset_cpus_str.empty()) {
        if (!set_cpuset(cpuset_cpus_str)) {
            valid_ = false;
            return false;
        }
    }

    valid_ = true;
    return true;
}

bool CgroupSlice::write_file(const std::string& filename, const std::string& value) {
    if (path_.empty()) {
        return false;
    }

    std::filesystem::path p(path_);
    p /= filename;
    std::string full_path = p.string();

    int fd = ::open(full_path.c_str(), O_WRONLY | O_CLOEXEC | O_CREAT, 0644);
    if (fd < 0) {
        std::fprintf(stderr,
                     "warning: [cgroup] failed to open '%s': %s (running in unprivileged fallback mode)\n",
                     full_path.c_str(), std::strerror(errno));
        return false;
    }

    ScopedFd scoped(fd);
    ssize_t written = ::write(scoped.get(), value.data(), value.size());
    if (written < 0 || static_cast<size_t>(written) != value.size()) {
        std::fprintf(stderr,
                     "warning: [cgroup] failed to write to '%s': %s (running in unprivileged fallback mode)\n",
                     full_path.c_str(), std::strerror(errno));
        return false;
    }

    return true;
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
    std::string val = (max_pids == 0) ? "max\n" : (std::to_string(max_pids) + "\n");
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

const std::string& CgroupSlice::path() const noexcept {
    return path_;
}

const std::string& CgroupSlice::name() const noexcept {
    return name_;
}

bool CgroupSlice::is_valid() const noexcept {
    return valid_;
}

bool CgroupSlice::valid() const noexcept {
    return valid_;
}

} // namespace virmux
