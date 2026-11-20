/**
 * @file bench.cpp
 * @brief Implementation of synthetic benchmark workloads, input latency probe,
 * and telemetry sampler.
 */

#include "virmux/bench.hpp"

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>

#include "virmux/cli_stats.hpp"
#include "virmux/gate.hpp"
#include "virmux/sandbox.hpp"
#include "virmux/sha256.hpp"

namespace virmux {

std::string_view workload_type_to_string(WorkloadType type) {
  switch (type) {
    case WorkloadType::BROWSER:
      return "browser";
    case WorkloadType::COMPILER:
      return "compiler";
    case WorkloadType::MODEL:
      return "model";
    case WorkloadType::BUILD:
      return "build";
  }
  return "unknown";
}

WorkloadType workload_type_from_string(std::string_view sv) {
  std::string lower;
  lower.reserve(sv.size());
  for (char c : sv) {
    lower.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  if (lower == "browser") {
    return WorkloadType::BROWSER;
  }
  if (lower == "compiler") {
    return WorkloadType::COMPILER;
  }
  if (lower == "model") {
    return WorkloadType::MODEL;
  }
  if (lower == "build") {
    return WorkloadType::BUILD;
  }
  throw std::invalid_argument("Unknown workload type: " + std::string(sv));
}

namespace {

bool check_should_stop(const std::chrono::steady_clock::time_point& start_time,
                       int duration_seconds,
                       const std::atomic<bool>* stop_flag) {
  if (stop_flag && stop_flag->load(std::memory_order_relaxed)) {
    return true;
  }
  if (duration_seconds > 0) {
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now() - start_time)
                       .count();
    if (elapsed >= duration_seconds) {
      return true;
    }
  }
  return false;
}

int run_browser_workload(int duration_seconds, uint64_t target_alloc_mb,
                         std::atomic<bool>* stop_flag) {
  const auto start_time = std::chrono::steady_clock::now();
  const uint64_t target_bytes = target_alloc_mb > 0
                                    ? (target_alloc_mb * 1024ULL * 1024ULL)
                                    : (16ULL * 1024ULL * 1024ULL);
  constexpr size_t kChunkSize = 64 * 1024;  // 64 KiB chunks
  const size_t num_chunks = std::max<size_t>(1, target_bytes / kChunkSize);

  std::vector<std::vector<char>> pages;
  pages.reserve(num_chunks);

  try {
    for (size_t i = 0; i < num_chunks; ++i) {
      if (stop_flag && stop_flag->load(std::memory_order_relaxed)) {
        break;
      }
      std::vector<char> chunk(kChunkSize);
      // Fill chunk with pseudo-DOM HTML structure for realistic anonymous pages
      const std::string node_prefix = "<div class=\"dom-node\" id=\"node_" +
                                      std::to_string(i) +
                                      "\"><span>Content block for virtual DOM "
                                      "tree layout element</span></div>\n";
      size_t pos = 0;
      while (pos + node_prefix.size() <= kChunkSize) {
        std::memcpy(chunk.data() + pos, node_prefix.data(), node_prefix.size());
        pos += node_prefix.size();
      }
      // Touch every 4 KiB page to fault it in
      for (size_t p = 0; p < kChunkSize; p += 4096) {
        chunk[p] = '<';
      }
      pages.push_back(std::move(chunk));
    }
  } catch (const std::bad_alloc&) {
    // Gracefully handle system memory pressure
  }

  // Periodic DOM scanning and mutation loop
  size_t scan_idx = 0;
  while (!check_should_stop(start_time, duration_seconds, stop_flag)) {
    if (!pages.empty()) {
      for (size_t s = 0; s < 64 && scan_idx < pages.size(); ++s, ++scan_idx) {
        auto& chunk = pages[scan_idx % pages.size()];
        if (!chunk.empty()) {
          chunk[0] = static_cast<char>('A' + (scan_idx % 26));
          volatile char v = chunk[chunk.size() - 1];
          (void)v;
        }
      }
      if (scan_idx >= pages.size()) {
        scan_idx = 0;
      }
    }
    if (duration_seconds <= 0 && stop_flag == nullptr) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  return 0;
}

int run_compiler_workload(int duration_seconds, uint64_t target_alloc_mb,
                          std::atomic<bool>* stop_flag) {
  (void)target_alloc_mb;
  const auto start_time = std::chrono::steady_clock::now();
  const unsigned int num_threads =
      std::clamp(std::thread::hardware_concurrency(), 1u, 4u);

  std::vector<std::thread> workers;
  workers.reserve(num_threads);

  for (unsigned int tid = 0; tid < num_threads; ++tid) {
    workers.emplace_back([start_time, duration_seconds, stop_flag, tid]() {
      uint64_t hash_accumulator = 0x811c9dc5ULL;
      const std::string synthetic_code =
          "int compute_block_" + std::to_string(tid) +
          "(int a, int b) { int sum = 0; for (int i = 0; i < 100; ++i) sum += "
          "(a * i + b) ^ 0x5a; return sum; }";

      while (!check_should_stop(start_time, duration_seconds, stop_flag)) {
        // Token parsing and symbol hashing
        for (char c : synthetic_code) {
          hash_accumulator ^= static_cast<uint64_t>(c);
          hash_accumulator *= 0x100000001b3ULL;
        }

        // CPU burst: integer and floating point math loop
        double math_burst = 1.0;
        for (int k = 0; k < 2000; ++k) {
          math_burst =
              std::sin(math_burst + static_cast<double>(k) * 0.001) * 1.0001;
        }
        volatile double dummy =
            math_burst + static_cast<double>(hash_accumulator);
        (void)dummy;

        if (duration_seconds <= 0 && stop_flag == nullptr) {
          break;
        }
        std::this_thread::yield();
      }
    });
  }

  for (auto& w : workers) {
    if (w.joinable()) {
      w.join();
    }
  }

  return 0;
}

int run_model_workload(int duration_seconds, uint64_t target_alloc_mb,
                       std::atomic<bool>* stop_flag) {
  const auto start_time = std::chrono::steady_clock::now();
  constexpr size_t N = 128;  // 128x128 matrix = 16384 floats = 64 KiB
  std::vector<float> a_mat(N * N, 0.5f);
  std::vector<float> b_mat(N * N, 0.25f);
  std::vector<float> c_mat(N * N, 0.0f);

  const size_t working_buf_bytes =
      target_alloc_mb > 0
          ? std::min<size_t>(target_alloc_mb * 1024ULL * 1024ULL,
                             64ULL * 1024ULL * 1024ULL)
          : (4ULL * 1024ULL * 1024ULL);

  while (!check_should_stop(start_time, duration_seconds, stop_flag)) {
    // Matrix multiplication: C = A * B
    for (size_t i = 0; i < N; ++i) {
      for (size_t k = 0; k < N; ++k) {
        const float aik = a_mat[i * N + k];
        for (size_t j = 0; j < N; ++j) {
          c_mat[i * N + j] += aik * b_mat[k * N + j];
        }
      }
    }

    // ReLU activation & normalization
    for (size_t idx = 0; idx < N * N; ++idx) {
      c_mat[idx] = std::max(0.0f, c_mat[idx]);
      if (c_mat[idx] > 100.0f) {
        c_mat[idx] = 1.0f;
      }
    }

    // Working buffer allocation and compaction cycle
    try {
      std::vector<uint8_t> working_buf(working_buf_bytes, 0x5a);
      if (!working_buf.empty()) {
        working_buf[0] = static_cast<uint8_t>(c_mat[0]);
        volatile uint8_t v = working_buf[working_buf.size() / 2];
        (void)v;
      }
    } catch (const std::bad_alloc&) {
    }

    if (duration_seconds <= 0 && stop_flag == nullptr) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  return 0;
}

int run_build_workload(int duration_seconds, uint64_t target_alloc_mb,
                       std::atomic<bool>* stop_flag) {
  (void)target_alloc_mb;
  const auto start_time = std::chrono::steady_clock::now();

  std::string dir_str = "/tmp/virmux_bench_build_" + std::to_string(::getpid());
  std::error_code ec;
  std::filesystem::remove_all(dir_str, ec);
  if (!std::filesystem::create_directories(dir_str, ec)) {
    return -1;
  }
  const std::filesystem::path temp_dir(dir_str);

  struct TempDirGuard {
    std::filesystem::path dir;
    ~TempDirGuard() {
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
    }
  } guard{temp_dir};

  size_t iteration = 0;
  constexpr size_t kBlockSize = 64 * 1024;  // 64 KiB
  std::vector<uint8_t> payload(kBlockSize, 0xcd);

  while (!check_should_stop(start_time, duration_seconds, stop_flag)) {
    const std::filesystem::path obj_path =
        temp_dir / ("obj_" + std::to_string(iteration) + ".o");

    // 1. Write streaming object data
    {
      std::ofstream ofs(obj_path, std::ios::binary);
      if (ofs.is_open()) {
        payload[0] = static_cast<uint8_t>(iteration & 0xff);
        ofs.write(reinterpret_cast<const char*>(payload.data()),
                  static_cast<std::streamsize>(payload.size()));
      }
    }

    // 2. Compute SHA-256 digest of object file
    const std::string digest = sha256_hash_file(obj_path.string());

    // 3. Stage synthetic CAS manifest entry
    const std::filesystem::path manifest_path = temp_dir / "build_manifest.cas";
    {
      std::ofstream mfs(manifest_path, std::ios::app);
      if (mfs.is_open()) {
        mfs << obj_path.filename().string() << " " << digest << "\n";
      }
    }

    // 4. Unlink object file
    std::error_code ec;
    std::filesystem::remove(obj_path, ec);

    iteration++;

    if (duration_seconds <= 0 && stop_flag == nullptr) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  return 0;
}

}  // namespace

int run_synthetic_workload(WorkloadType type, int duration_seconds,
                           uint64_t target_alloc_mb,
                           std::atomic<bool>* stop_flag) {
  switch (type) {
    case WorkloadType::BROWSER:
      return run_browser_workload(duration_seconds, target_alloc_mb, stop_flag);
    case WorkloadType::COMPILER:
      return run_compiler_workload(duration_seconds, target_alloc_mb,
                                   stop_flag);
    case WorkloadType::MODEL:
      return run_model_workload(duration_seconds, target_alloc_mb, stop_flag);
    case WorkloadType::BUILD:
      return run_build_workload(duration_seconds, target_alloc_mb, stop_flag);
  }
  return -1;
}

// ---------------------------------------------------------------------------
// InputLatencyProbe Implementation
// ---------------------------------------------------------------------------

InputLatencyProbe::InputLatencyProbe(bool force_pipe_fallback)
    : force_pipe_fallback_(force_pipe_fallback) {
  open_descriptors();
}

InputLatencyProbe::~InputLatencyProbe() {
  stop();
  close_descriptors();
}

InputLatencyProbe::InputLatencyProbe(InputLatencyProbe&& other) noexcept {
  other.stop();
  std::scoped_lock lock(samples_mutex_, other.samples_mutex_, io_mutex_,
                        other.io_mutex_);
  force_pipe_fallback_ = other.force_pipe_fallback_;
  is_pty_ = other.is_pty_;
  master_fd_ = other.master_fd_;
  slave_fd_ = other.slave_fd_;
  samples_ = std::move(other.samples_);
  other.master_fd_ = -1;
  other.slave_fd_ = -1;
}

InputLatencyProbe& InputLatencyProbe::operator=(
    InputLatencyProbe&& other) noexcept {
  if (this != &other) {
    stop();
    close_descriptors();
    other.stop();
    std::scoped_lock lock(samples_mutex_, other.samples_mutex_, io_mutex_,
                          other.io_mutex_);
    force_pipe_fallback_ = other.force_pipe_fallback_;
    is_pty_ = other.is_pty_;
    master_fd_ = other.master_fd_;
    slave_fd_ = other.slave_fd_;
    samples_ = std::move(other.samples_);
    other.master_fd_ = -1;
    other.slave_fd_ = -1;
  }
  return *this;
}

bool InputLatencyProbe::open_descriptors() {
  close_descriptors();

  if (!force_pipe_fallback_) {
    int m = -1;
    int s = -1;
    if (::openpty(&m, &s, nullptr, nullptr, nullptr) == 0) {
      master_fd_ = m;
      slave_fd_ = s;
      is_pty_ = true;

      int m_flags = ::fcntl(master_fd_, F_GETFL, 0);
      ::fcntl(master_fd_, F_SETFL, m_flags | O_NONBLOCK);
      int s_flags = ::fcntl(slave_fd_, F_GETFL, 0);
      ::fcntl(slave_fd_, F_SETFL, s_flags | O_NONBLOCK);

      struct termios tios{};
      if (::tcgetattr(slave_fd_, &tios) == 0) {
        tios.c_lflag &= ~ICANON;
        tios.c_lflag |= ECHO;
        ::tcsetattr(slave_fd_, TCSANOW, &tios);
      }
      return true;
    }
  }

  // Fallback: anonymous non-blocking pipe
  int fds[2] = {-1, -1};
  if (::pipe(fds) == 0) {
    master_fd_ = fds[1];  // write end
    slave_fd_ = fds[0];   // read end
    is_pty_ = false;

    int w_flags = ::fcntl(master_fd_, F_GETFL, 0);
    ::fcntl(master_fd_, F_SETFL, w_flags | O_NONBLOCK);
    int r_flags = ::fcntl(slave_fd_, F_GETFL, 0);
    ::fcntl(slave_fd_, F_SETFL, r_flags | O_NONBLOCK);
    return true;
  }

  return false;
}

void InputLatencyProbe::close_descriptors() {
  if (master_fd_ >= 0) {
    ::close(master_fd_);
    master_fd_ = -1;
  }
  if (slave_fd_ >= 0) {
    ::close(slave_fd_);
    slave_fd_ = -1;
  }
}

double InputLatencyProbe::measure_sample() {
  std::lock_guard<std::mutex> io_lock(io_mutex_);

  if (master_fd_ < 0 || slave_fd_ < 0) {
    if (!open_descriptors()) {
      return -1.0;
    }
  }

  const int write_fd = master_fd_;
  const int read_fd = is_pty_ ? master_fd_ : slave_fd_;

  char drain_buf[128];
  while (::read(read_fd, drain_buf, sizeof(drain_buf)) > 0) {
  }
  if (is_pty_) {
    ::tcflush(slave_fd_, TCIFLUSH);
  }

  const auto t0 = std::chrono::steady_clock::now();
  const char token = 'P';
  ssize_t nw = ::write(write_fd, &token, 1);
  if (nw <= 0) {
    return -1.0;
  }

  struct pollfd pfd{};
  pfd.fd = read_fd;
  pfd.events = POLLIN;

  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  int ret = 0;
  while (true) {
    auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                   deadline - std::chrono::steady_clock::now())
                   .count();
    if (rem <= 0) {
      ret = 0;
      break;
    }
    ret = ::poll(&pfd, 1, static_cast<int>(rem));
    if (ret < 0 && errno == EINTR) {
      continue;
    }
    break;
  }

  if (ret > 0 && (pfd.revents & POLLIN)) {
    char resp = 0;
    ssize_t nr = ::read(read_fd, &resp, 1);
    if (nr > 0) {
      const auto t1 = std::chrono::steady_clock::now();
      const double latency_ms =
          std::chrono::duration<double, std::milli>(t1 - t0).count();

      if (is_pty_) {
        ::tcflush(slave_fd_, TCIFLUSH);
      }

      std::lock_guard<std::mutex> lock(samples_mutex_);
      samples_.push_back(latency_ms);
      return latency_ms;
    }
  }

  return -1.0;
}

void InputLatencyProbe::start(int interval_ms) {
  std::lock_guard<std::mutex> lock(samples_mutex_);
  if (running_) {
    return;
  }
  if (master_fd_ < 0 || slave_fd_ < 0) {
    open_descriptors();
  }
  running_ = true;
  worker_ = std::thread([this, interval_ms]() {
    while (running_) {
      measure_sample();
      std::unique_lock<std::mutex> lk(samples_mutex_);
      cv_.wait_for(lk, std::chrono::milliseconds(interval_ms),
                   [this]() { return !running_; });
    }
  });
}

void InputLatencyProbe::stop() {
  {
    std::lock_guard<std::mutex> lock(samples_mutex_);
    if (!running_) {
      return;
    }
    running_ = false;
    cv_.notify_all();
  }
  if (worker_.joinable()) {
    worker_.join();
  }
}

double InputLatencyProbe::avg_latency_ms() const {
  std::lock_guard<std::mutex> lock(samples_mutex_);
  if (samples_.empty()) {
    return 0.0;
  }
  double sum = std::accumulate(samples_.begin(), samples_.end(), 0.0);
  return sum / static_cast<double>(samples_.size());
}

double InputLatencyProbe::max_latency_ms() const {
  std::lock_guard<std::mutex> lock(samples_mutex_);
  if (samples_.empty()) {
    return 0.0;
  }
  return *std::max_element(samples_.begin(), samples_.end());
}

double InputLatencyProbe::min_latency_ms() const {
  std::lock_guard<std::mutex> lock(samples_mutex_);
  if (samples_.empty()) {
    return 0.0;
  }
  return *std::min_element(samples_.begin(), samples_.end());
}

double InputLatencyProbe::p99_latency_ms() const {
  std::lock_guard<std::mutex> lock(samples_mutex_);
  if (samples_.empty()) {
    return 0.0;
  }
  std::vector<double> sorted = samples_;
  std::sort(sorted.begin(), sorted.end());
  if (sorted.size() == 1) {
    return sorted[0];
  }
  size_t idx = static_cast<size_t>(
                   std::ceil(0.99 * static_cast<double>(sorted.size()))) -
               1;
  if (idx >= sorted.size()) {
    idx = sorted.size() - 1;
  }
  return sorted[idx];
}

size_t InputLatencyProbe::sample_count() const {
  std::lock_guard<std::mutex> lock(samples_mutex_);
  return samples_.size();
}

bool InputLatencyProbe::is_running() const { return running_.load(); }

bool InputLatencyProbe::is_pty() const { return is_pty_; }

void InputLatencyProbe::reset() {
  std::lock_guard<std::mutex> lock(samples_mutex_);
  samples_.clear();
}

// ---------------------------------------------------------------------------
// TelemetrySampler Implementation
// ---------------------------------------------------------------------------

TelemetrySampler::TelemetrySampler(std::string procfs_root,
                                   std::string ksm_sysfs_root)
    : procfs_root_(std::move(procfs_root)),
      ksm_sysfs_root_(std::move(ksm_sysfs_root)),
      mem_telemetry_(procfs_root_, "/sys/block"),
      ksm_controller_(ksm_sysfs_root_) {}

TelemetrySampler::~TelemetrySampler() { stop(); }

TelemetrySampler::TelemetrySampler(TelemetrySampler&& other) noexcept {
  other.stop();
  std::lock_guard<std::mutex> lock(other.mutex_);
  procfs_root_ = std::move(other.procfs_root_);
  ksm_sysfs_root_ = std::move(other.ksm_sysfs_root_);
  mem_telemetry_ = std::move(other.mem_telemetry_);
  ksm_controller_ = std::move(other.ksm_controller_);
  tracked_pids_ = std::move(other.tracked_pids_);
  current_rss_ = other.current_rss_;
  current_swap_ = other.current_swap_;
  peak_rss_ = other.peak_rss_;
  peak_swap_ = other.peak_swap_;
  mem_total_ = other.mem_total_;
  mem_free_ = other.mem_free_;
  mem_available_ = other.mem_available_;
  swap_total_ = other.swap_total_;
  swap_free_ = other.swap_free_;
  ksm_bytes_saved_ = other.ksm_bytes_saved_;
  peak_ksm_bytes_saved_ = other.peak_ksm_bytes_saved_;
  sample_count_ = other.sample_count_;
}

TelemetrySampler& TelemetrySampler::operator=(
    TelemetrySampler&& other) noexcept {
  if (this != &other) {
    stop();
    other.stop();
    std::scoped_lock lock(mutex_, other.mutex_);
    procfs_root_ = std::move(other.procfs_root_);
    ksm_sysfs_root_ = std::move(other.ksm_sysfs_root_);
    mem_telemetry_ = std::move(other.mem_telemetry_);
    ksm_controller_ = std::move(other.ksm_controller_);
    tracked_pids_ = std::move(other.tracked_pids_);
    current_rss_ = other.current_rss_;
    current_swap_ = other.current_swap_;
    peak_rss_ = other.peak_rss_;
    peak_swap_ = other.peak_swap_;
    mem_total_ = other.mem_total_;
    mem_free_ = other.mem_free_;
    mem_available_ = other.mem_available_;
    swap_total_ = other.swap_total_;
    swap_free_ = other.swap_free_;
    ksm_bytes_saved_ = other.ksm_bytes_saved_;
    peak_ksm_bytes_saved_ = other.peak_ksm_bytes_saved_;
    sample_count_ = other.sample_count_;
  }
  return *this;
}

void TelemetrySampler::sample() {
  std::lock_guard<std::mutex> lock(mutex_);

  const auto host_stats = mem_telemetry_.read_host_stats();
  mem_total_ = host_stats.mem_total_bytes;
  mem_free_ = host_stats.mem_free_bytes;
  mem_available_ = host_stats.mem_available_bytes;
  swap_total_ = host_stats.swap_total_bytes;
  swap_free_ = host_stats.swap_free_bytes;

  if (!tracked_pids_.empty()) {
    uint64_t sum_rss = 0;
    uint64_t sum_swap = 0;
    for (pid_t pid : tracked_pids_) {
      if (pid > 0) {
        const auto pstats = mem_telemetry_.read_process_stats(pid);
        sum_rss += pstats.rss_bytes;
        sum_swap += pstats.swap_bytes;
      }
    }
    current_rss_ = sum_rss;
    current_swap_ = sum_swap;
  } else {
    if (mem_total_ >= mem_available_ && mem_available_ > 0) {
      current_rss_ = mem_total_ - mem_available_;
    } else if (mem_total_ >= mem_free_) {
      current_rss_ = mem_total_ - mem_free_;
    } else {
      current_rss_ = 0;
    }

    if (swap_total_ >= swap_free_) {
      current_swap_ = swap_total_ - swap_free_;
    } else {
      current_swap_ = 0;
    }
  }

  if (current_rss_ > peak_rss_) {
    peak_rss_ = current_rss_;
  }
  if (current_swap_ > peak_swap_) {
    peak_swap_ = current_swap_;
  }

  const auto ksm = ksm_controller_.read_stats();
  ksm_bytes_saved_ = ksm.bytes_saved;
  if (ksm_bytes_saved_ > peak_ksm_bytes_saved_) {
    peak_ksm_bytes_saved_ = ksm_bytes_saved_;
  }

  sample_count_++;
}

void TelemetrySampler::start(int interval_ms) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (running_) {
    return;
  }
  running_ = true;
  worker_ = std::thread([this, interval_ms]() {
    while (running_) {
      sample();
      std::unique_lock<std::mutex> lk(mutex_);
      cv_.wait_for(lk, std::chrono::milliseconds(interval_ms),
                   [this]() { return !running_; });
    }
  });
}

void TelemetrySampler::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) {
      return;
    }
    running_ = false;
    cv_.notify_all();
  }
  if (worker_.joinable()) {
    worker_.join();
  }
}

uint64_t TelemetrySampler::current_rss_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_rss_;
}

uint64_t TelemetrySampler::current_swap_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_swap_;
}

uint64_t TelemetrySampler::peak_rss_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return peak_rss_;
}

uint64_t TelemetrySampler::peak_swap_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return peak_swap_;
}

uint64_t TelemetrySampler::mem_total_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mem_total_;
}

uint64_t TelemetrySampler::mem_free_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mem_free_;
}

uint64_t TelemetrySampler::mem_available_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return mem_available_;
}

uint64_t TelemetrySampler::swap_total_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return swap_total_;
}

uint64_t TelemetrySampler::swap_free_bytes() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return swap_free_;
}

uint64_t TelemetrySampler::ksm_bytes_saved() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return ksm_bytes_saved_;
}

uint64_t TelemetrySampler::peak_ksm_bytes_saved() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return peak_ksm_bytes_saved_;
}

size_t TelemetrySampler::sample_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sample_count_;
}

bool TelemetrySampler::is_running() const { return running_.load(); }

void TelemetrySampler::set_target_pids(const std::vector<pid_t>& pids) {
  std::lock_guard<std::mutex> lock(mutex_);
  tracked_pids_ = pids;
}

void TelemetrySampler::add_target_pid(pid_t pid) {
  std::lock_guard<std::mutex> lock(mutex_);
  tracked_pids_.push_back(pid);
}

void TelemetrySampler::clear_target_pids() {
  std::lock_guard<std::mutex> lock(mutex_);
  tracked_pids_.clear();
}

namespace {

std::string_view trim_ws(std::string_view s) {
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

/**
 * @brief Return the index just past the closing quote of the JSON string
 * literal whose opening quote is at @p open, honouring backslash escapes.
 * @return Index after the closing quote, or npos if unterminated.
 */
size_t skip_json_string(std::string_view json, size_t open) {
  for (size_t i = open + 1; i < json.size(); ++i) {
    if (json[i] == '\\') {
      ++i;  // Skip the escaped character, whatever it is.
    } else if (json[i] == '"') {
      return i + 1;
    }
  }
  return std::string_view::npos;
}

/**
 * @brief Escape arbitrary bytes for embedding in a JSON string literal.
 */
std::string escape_json_string(std::string_view in) {
  std::string out;
  out.reserve(in.size() + 2);
  for (const char c : in) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(
              buf, sizeof(buf), "\\u%04x",
              static_cast<unsigned int>(static_cast<unsigned char>(c)));
          out += buf;
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

std::optional<std::string_view> extract_json_obj(std::string_view json,
                                                 std::string_view key) {
  std::string quoted = "\"" + std::string(key) + "\"";
  size_t pos = json.find(quoted);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  pos = json.find(':', pos + quoted.size());
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }
  pos = json.find('{', pos + 1);
  if (pos == std::string_view::npos) {
    return std::nullopt;
  }

  size_t start = pos;
  int depth = 0;
  for (size_t i = start; i < json.size(); ++i) {
    if (json[i] == '"') {
      // Braces inside string values do not affect nesting depth.
      size_t end = skip_json_string(json, i);
      if (end == std::string_view::npos) {
        return std::nullopt;
      }
      i = end - 1;
    } else if (json[i] == '{') {
      depth++;
    } else if (json[i] == '}') {
      depth--;
      if (depth == 0) {
        return json.substr(start, i - start + 1);
      }
    }
  }
  return std::nullopt;
}

std::string parse_str_field(std::string_view obj, std::string_view key,
                            const std::string& def = "") {
  std::string quoted = "\"" + std::string(key) + "\"";
  size_t pos = obj.find(quoted);
  if (pos == std::string_view::npos) {
    return def;
  }
  pos = obj.find(':', pos + quoted.size());
  if (pos == std::string_view::npos) {
    return def;
  }
  pos = obj.find('"', pos + 1);
  if (pos == std::string_view::npos) {
    return def;
  }
  // Decode up to the first unescaped quote. Escapes mirror
  // escape_json_string(): \u00XX decodes to the single byte XX; any other
  // \uXXXX sequence is kept verbatim.
  std::string val;
  for (size_t i = pos + 1; i < obj.size(); ++i) {
    const char c = obj[i];
    if (c == '"') {
      return val;
    }
    if (c != '\\') {
      val += c;
      continue;
    }
    if (++i >= obj.size()) {
      break;
    }
    switch (obj[i]) {
      case 'n':
        val += '\n';
        break;
      case 'r':
        val += '\r';
        break;
      case 't':
        val += '\t';
        break;
      case 'b':
        val += '\b';
        break;
      case 'f':
        val += '\f';
        break;
      case 'u': {
        unsigned int cp = 0;
        const std::string_view hex = obj.substr(i + 1, 4);
        const auto [ptr, ec] =
            std::from_chars(hex.data(), hex.data() + hex.size(), cp, 16);
        if (hex.size() == 4 && ec == std::errc() &&
            ptr == hex.data() + hex.size() && cp <= 0xFF) {
          val += static_cast<char>(cp);
          i += 4;
        } else {
          val += "\\u";
        }
        break;
      }
      default:
        val += obj[i];  // Covers \" \\ \/ and unknown escapes.
        break;
    }
  }
  return def;  // Unterminated string literal.
}

double parse_double_field(std::string_view obj, std::string_view key,
                          double def = 0.0) {
  std::string quoted = "\"" + std::string(key) + "\"";
  size_t pos = obj.find(quoted);
  if (pos == std::string_view::npos) {
    return def;
  }
  pos = obj.find(':', pos + quoted.size());
  if (pos == std::string_view::npos) {
    return def;
  }
  pos++;
  while (pos < obj.size() &&
         std::isspace(static_cast<unsigned char>(obj[pos]))) {
    pos++;
  }
  size_t end = pos;
  while (end < obj.size() &&
         (std::isdigit(static_cast<unsigned char>(obj[end])) ||
          obj[end] == '.' || obj[end] == '-' || obj[end] == 'e' ||
          obj[end] == 'E' || obj[end] == '+')) {
    end++;
  }
  if (end > pos) {
    try {
      return std::stod(std::string(obj.substr(pos, end - pos)));
    } catch (...) {
    }
  }
  return def;
}

uint64_t parse_uint64_field(std::string_view obj, std::string_view key,
                            uint64_t def = 0) {
  std::string quoted = "\"" + std::string(key) + "\"";
  size_t pos = obj.find(quoted);
  if (pos == std::string_view::npos) {
    return def;
  }
  pos = obj.find(':', pos + quoted.size());
  if (pos == std::string_view::npos) {
    return def;
  }
  pos++;
  while (pos < obj.size() &&
         std::isspace(static_cast<unsigned char>(obj[pos]))) {
    pos++;
  }
  size_t end = pos;
  while (end < obj.size() &&
         std::isdigit(static_cast<unsigned char>(obj[end]))) {
    end++;
  }
  if (end > pos) {
    try {
      return std::stoull(std::string(obj.substr(pos, end - pos)));
    } catch (...) {
    }
  }
  return def;
}

int parse_int_field(std::string_view obj, std::string_view key, int def = 0) {
  const double v = parse_double_field(obj, key, static_cast<double>(def));
  // double -> int truncation is UB unless the truncated value fits in int,
  // i.e. v lies strictly inside (INT_MIN - 1, INT_MAX + 1). Both bounds are
  // exactly representable as doubles. NaN fails both comparisons.
  constexpr double kLower =
      static_cast<double>(std::numeric_limits<int>::min()) - 1.0;
  constexpr double kUpper =
      static_cast<double>(std::numeric_limits<int>::max()) + 1.0;
  if (!(v > kLower && v < kUpper)) {
    return def;
  }
  return static_cast<int>(v);
}

BenchmarkResult parse_result_block(std::string_view obj) {
  BenchmarkResult res;
  res.mode = parse_str_field(obj, "mode", "baseline");
  res.task_count = parse_int_field(obj, "task_count", 0);
  res.wall_time_sec = parse_double_field(obj, "wall_time_sec", 0.0);
  res.peak_rss_bytes = parse_uint64_field(obj, "peak_rss_bytes", 0);
  res.peak_swap_bytes = parse_uint64_field(obj, "peak_swap_bytes", 0);
  res.peak_ksm_bytes_saved = parse_uint64_field(obj, "peak_ksm_bytes_saved", 0);
  res.avg_latency_ms = parse_double_field(obj, "avg_latency_ms", 0.0);
  res.p99_latency_ms = parse_double_field(obj, "p99_latency_ms", 0.0);
  res.min_latency_ms = parse_double_field(obj, "min_latency_ms", 0.0);
  res.max_latency_ms = parse_double_field(obj, "max_latency_ms", 0.0);
  res.latency_samples =
      static_cast<size_t>(parse_uint64_field(obj, "latency_samples", 0));
  res.oom_killed_count = parse_int_field(obj, "oom_killed_count", 0);
  res.successful_tasks = parse_int_field(obj, "successful_tasks", 0);
  res.failed_tasks = parse_int_field(obj, "failed_tasks", 0);
  return res;
}

}  // namespace

std::string format_comparison_table(const BenchmarkComparison& comp) {
  std::ostringstream oss;
  constexpr int kWidth = 88;

  if (comp.baseline.task_count == 0 && comp.managed.task_count == 0) {
    oss << "No benchmark results recorded.\n";
    return oss.str();
  }

  if (comp.baseline.task_count == 0 && comp.managed.task_count > 0) {
    const auto& m = comp.managed;
    oss << std::string(kWidth, '=') << "\n";
    oss << "                       VIRMUX GAUNTLET BENCHMARK REPORT\n";
    oss << std::string(kWidth, '=') << "\n";
    oss << "  Execution Mode:             Virmux Managed\n";
    oss << "  Concurrent Tasks:           " << m.task_count << "\n";
    oss << "  Total Wall-Clock Time:      " << std::fixed
        << std::setprecision(2) << m.wall_time_sec << " s\n";
    oss << "  Peak Host RAM (RSS):        " << format_bytes(m.peak_rss_bytes)
        << "\n";
    oss << "  Peak Swap Allocated:        " << format_bytes(m.peak_swap_bytes)
        << "\n";
    oss << "  KSM Memory Saved:           "
        << format_bytes(m.peak_ksm_bytes_saved) << "\n";
    oss << "  Host Terminal Input Lag:    " << std::fixed
        << std::setprecision(2) << m.avg_latency_ms
        << " ms (p99: " << m.p99_latency_ms << " ms)\n";
    oss << "  OOM Termination Count:      " << m.oom_killed_count << "\n";
    oss << "  Tasks Completed / Total:    " << m.successful_tasks << "/"
        << m.task_count << "\n";
    oss << std::string(kWidth, '=') << "\n";
    return oss.str();
  }

  if (comp.managed.task_count == 0 && comp.baseline.task_count > 0) {
    const auto& b = comp.baseline;
    oss << std::string(kWidth, '=') << "\n";
    oss << "                       VIRMUX GAUNTLET BENCHMARK REPORT\n";
    oss << std::string(kWidth, '=') << "\n";
    oss << "  Execution Mode:             Unmanaged Baseline\n";
    oss << "  Concurrent Tasks:           " << b.task_count << "\n";
    oss << "  Total Wall-Clock Time:      " << std::fixed
        << std::setprecision(2) << b.wall_time_sec << " s\n";
    oss << "  Peak Host RAM (RSS):        " << format_bytes(b.peak_rss_bytes)
        << "\n";
    oss << "  Peak Swap Allocated:        " << format_bytes(b.peak_swap_bytes)
        << "\n";
    oss << "  KSM Memory Saved:           "
        << format_bytes(b.peak_ksm_bytes_saved) << "\n";
    oss << "  Host Terminal Input Lag:    " << std::fixed
        << std::setprecision(2) << b.avg_latency_ms
        << " ms (p99: " << b.p99_latency_ms << " ms)\n";
    oss << "  OOM Termination Count:      " << b.oom_killed_count << "\n";
    oss << "  Tasks Completed / Total:    " << b.successful_tasks << "/"
        << b.task_count << "\n";
    oss << std::string(kWidth, '=') << "\n";
    return oss.str();
  }

  const auto& b = comp.baseline;
  const auto& m = comp.managed;

  char buf[256];
  oss << std::string(kWidth, '=') << "\n";
  oss << "                       VIRMUX 12-TERMINAL GAUNTLET BENCHMARK "
         "REPORT\n";
  oss << std::string(kWidth, '=') << "\n";
  std::snprintf(buf, sizeof(buf), "%-28s %-24s %-24s %s\n", "Metric",
                "Baseline (Unmanaged)", "Virmux Managed", "Delta / Gain");
  oss << buf;
  oss << std::string(kWidth, '-') << "\n";

  // 1. Peak Host RAM
  {
    double red = comp.rss_reduction_pct();
    std::string delta_str;
    if (b.peak_rss_bytes > 0) {
      char d_buf[64];
      std::snprintf(d_buf, sizeof(d_buf), "%+.1f%%", -red);
      delta_str = d_buf;
    } else {
      delta_str = "N/A";
    }
    std::snprintf(buf, sizeof(buf), "%-28s %-24s %-24s %s\n",
                  "Peak Host RAM (RSS)", format_bytes(b.peak_rss_bytes).c_str(),
                  format_bytes(m.peak_rss_bytes).c_str(), delta_str.c_str());
    oss << buf;
  }

  // 2. Peak Swap
  {
    double red = comp.swap_reduction_pct();
    std::string delta_str;
    if (b.peak_swap_bytes > 0) {
      char d_buf[64];
      std::snprintf(d_buf, sizeof(d_buf), "%+.1f%%", -red);
      delta_str = d_buf;
    } else {
      delta_str = "0 B (Clean)";
    }
    std::snprintf(buf, sizeof(buf), "%-28s %-24s %-24s %s\n",
                  "Peak Swap Allocated",
                  format_bytes(b.peak_swap_bytes).c_str(),
                  format_bytes(m.peak_swap_bytes).c_str(), delta_str.c_str());
    oss << buf;
  }

  // 3. KSM Memory Saved
  {
    int64_t ksm_delta = static_cast<int64_t>(m.peak_ksm_bytes_saved) -
                        static_cast<int64_t>(b.peak_ksm_bytes_saved);
    std::string gain = (ksm_delta >= 0 ? "+" : "-") +
                       format_bytes(std::abs(ksm_delta)) + " saved";
    std::snprintf(buf, sizeof(buf), "%-28s %-24s %-24s %s\n",
                  "KSM Memory Saved",
                  format_bytes(b.peak_ksm_bytes_saved).c_str(),
                  format_bytes(m.peak_ksm_bytes_saved).c_str(), gain.c_str());
    oss << buf;
  }

  // 4. Input Latency
  {
    char b_lat[64], m_lat[64], delta_lat[64];
    std::snprintf(b_lat, sizeof(b_lat), "%.2f ms (p99: %.2f)", b.avg_latency_ms,
                  b.p99_latency_ms);
    std::snprintf(m_lat, sizeof(m_lat), "%.2f ms (p99: %.2f)", m.avg_latency_ms,
                  m.p99_latency_ms);
    double lat_mult = comp.latency_improvement_x();
    std::snprintf(delta_lat, sizeof(delta_lat), "%.1fx faster", lat_mult);
    std::snprintf(buf, sizeof(buf), "%-28s %-24s %-24s %s\n",
                  "Host Terminal Input Lag", b_lat, m_lat, delta_lat);
    oss << buf;
  }

  // 5. Wall-Clock Completion
  {
    char b_time[64], m_time[64], delta_time[64];
    std::snprintf(b_time, sizeof(b_time), "%.2f s", b.wall_time_sec);
    std::snprintf(m_time, sizeof(m_time), "%.2f s", m.wall_time_sec);
    double t_mult = comp.time_improvement_x();
    std::snprintf(delta_time, sizeof(delta_time), "%.1fx faster", t_mult);
    std::snprintf(buf, sizeof(buf), "%-28s %-24s %-24s %s\n",
                  "Total Wall-Clock Time", b_time, m_time, delta_time);
    oss << buf;
  }

  // 6. OOM Termination Count
  {
    char b_oom[64], m_oom[64], delta_oom[64];
    std::snprintf(b_oom, sizeof(b_oom), "%d killed", b.oom_killed_count);
    std::snprintf(m_oom, sizeof(m_oom), "%d killed", m.oom_killed_count);
    if (m.oom_killed_count == 0) {
      std::snprintf(delta_oom, sizeof(delta_oom), "0 failures");
    } else {
      std::snprintf(delta_oom, sizeof(delta_oom), "%+d OOMs",
                    m.oom_killed_count - b.oom_killed_count);
    }
    std::snprintf(buf, sizeof(buf), "%-28s %-24s %-24s %s\n",
                  "OOM Termination Count", b_oom, m_oom, delta_oom);
    oss << buf;
  }

  // 7. Tasks Completed / Total
  {
    char b_tasks[64], m_tasks[64], delta_tasks[64];
    std::snprintf(b_tasks, sizeof(b_tasks), "%d/%d", b.successful_tasks,
                  b.task_count);
    std::snprintf(m_tasks, sizeof(m_tasks), "%d/%d", m.successful_tasks,
                  m.task_count);
    double pass_rate =
        m.task_count > 0 ? (100.0 * m.successful_tasks / m.task_count) : 0.0;
    std::snprintf(delta_tasks, sizeof(delta_tasks), "%.1f%% pass", pass_rate);
    std::snprintf(buf, sizeof(buf), "%-28s %-24s %-24s %s\n",
                  "Tasks Completed / Total", b_tasks, m_tasks, delta_tasks);
    oss << buf;
  }

  oss << std::string(kWidth, '=') << "\n";
  oss << "Summary: Peak RAM reduced by " << std::fixed << std::setprecision(1)
      << comp.rss_reduction_pct() << "%, input latency improved by "
      << std::fixed << std::setprecision(1) << comp.latency_improvement_x()
      << "x, completion time " << std::fixed << std::setprecision(1)
      << comp.time_improvement_x() << "x faster.\n";

  return oss.str();
}

std::string format_comparison_json(const BenchmarkComparison& comp) {
  std::ostringstream oss;
  oss << "{\n";
  oss << "  \"baseline\": {\n";
  oss << "    \"mode\": \"" << escape_json_string(comp.baseline.mode)
      << "\",\n";
  oss << "    \"task_count\": " << comp.baseline.task_count << ",\n";
  oss << "    \"wall_time_sec\": " << std::fixed << std::setprecision(4)
      << comp.baseline.wall_time_sec << ",\n";
  oss << "    \"peak_rss_bytes\": " << comp.baseline.peak_rss_bytes << ",\n";
  oss << "    \"peak_swap_bytes\": " << comp.baseline.peak_swap_bytes << ",\n";
  oss << "    \"peak_ksm_bytes_saved\": " << comp.baseline.peak_ksm_bytes_saved
      << ",\n";
  oss << "    \"avg_latency_ms\": " << comp.baseline.avg_latency_ms << ",\n";
  oss << "    \"p99_latency_ms\": " << comp.baseline.p99_latency_ms << ",\n";
  oss << "    \"min_latency_ms\": " << comp.baseline.min_latency_ms << ",\n";
  oss << "    \"max_latency_ms\": " << comp.baseline.max_latency_ms << ",\n";
  oss << "    \"latency_samples\": " << comp.baseline.latency_samples << ",\n";
  oss << "    \"oom_killed_count\": " << comp.baseline.oom_killed_count
      << ",\n";
  oss << "    \"successful_tasks\": " << comp.baseline.successful_tasks
      << ",\n";
  oss << "    \"failed_tasks\": " << comp.baseline.failed_tasks << "\n";
  oss << "  },\n";
  oss << "  \"managed\": {\n";
  oss << "    \"mode\": \"" << escape_json_string(comp.managed.mode) << "\",\n";
  oss << "    \"task_count\": " << comp.managed.task_count << ",\n";
  oss << "    \"wall_time_sec\": " << comp.managed.wall_time_sec << ",\n";
  oss << "    \"peak_rss_bytes\": " << comp.managed.peak_rss_bytes << ",\n";
  oss << "    \"peak_swap_bytes\": " << comp.managed.peak_swap_bytes << ",\n";
  oss << "    \"peak_ksm_bytes_saved\": " << comp.managed.peak_ksm_bytes_saved
      << ",\n";
  oss << "    \"avg_latency_ms\": " << comp.managed.avg_latency_ms << ",\n";
  oss << "    \"p99_latency_ms\": " << comp.managed.p99_latency_ms << ",\n";
  oss << "    \"min_latency_ms\": " << comp.managed.min_latency_ms << ",\n";
  oss << "    \"max_latency_ms\": " << comp.managed.max_latency_ms << ",\n";
  oss << "    \"latency_samples\": " << comp.managed.latency_samples << ",\n";
  oss << "    \"oom_killed_count\": " << comp.managed.oom_killed_count << ",\n";
  oss << "    \"successful_tasks\": " << comp.managed.successful_tasks << ",\n";
  oss << "    \"failed_tasks\": " << comp.managed.failed_tasks << "\n";
  oss << "  },\n";
  oss << "  \"summary\": {\n";
  oss << "    \"rss_reduction_pct\": " << comp.rss_reduction_pct() << ",\n";
  oss << "    \"swap_reduction_pct\": " << comp.swap_reduction_pct() << ",\n";
  oss << "    \"latency_improvement_x\": " << comp.latency_improvement_x()
      << ",\n";
  oss << "    \"time_improvement_x\": " << comp.time_improvement_x() << "\n";
  oss << "  }\n";
  oss << "}\n";
  return oss.str();
}

std::optional<BenchmarkComparison> parse_comparison_json(
    std::string_view json) {
  auto trimmed = trim_ws(json);
  if (trimmed.empty() || trimmed.front() != '{' || trimmed.back() != '}') {
    return std::nullopt;
  }

  auto baseline_obj = extract_json_obj(trimmed, "baseline");
  auto managed_obj = extract_json_obj(trimmed, "managed");

  if (!baseline_obj && !managed_obj) {
    return std::nullopt;
  }

  BenchmarkComparison comp;
  if (baseline_obj) {
    comp.baseline = parse_result_block(*baseline_obj);
  }
  if (managed_obj) {
    comp.managed = parse_result_block(*managed_obj);
  }
  return comp;
}

BenchmarkRunner::BenchmarkRunner(bool quick) : quick_(quick) {}

BenchmarkRunner::~BenchmarkRunner() { stop(); }

BenchmarkRunner::BenchmarkRunner(BenchmarkRunner&& other) noexcept
    : quick_(other.quick_), stop_flag_(other.stop_flag_.load()) {}

BenchmarkRunner& BenchmarkRunner::operator=(BenchmarkRunner&& other) noexcept {
  if (this != &other) {
    stop();
    quick_ = other.quick_;
    stop_flag_.store(other.stop_flag_.load());
  }
  return *this;
}

void BenchmarkRunner::request_stop() noexcept {
  stop_flag_.store(true, std::memory_order_relaxed);
}

void BenchmarkRunner::stop() {
  request_stop();
  reap_children();
}

void BenchmarkRunner::reap_children() {
  std::lock_guard<std::mutex> lock(pids_mutex_);
  for (pid_t p : active_pids_) {
    if (p > 0) {
      if (::kill(p, 0) == 0) {
        ::kill(-p, SIGTERM);
        ::kill(p, SIGTERM);
      }
      std::error_code ec;
      std::filesystem::remove_all(
          "/tmp/virmux_bench_build_" + std::to_string(p), ec);
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  for (pid_t p : active_pids_) {
    if (p > 0) {
      if (::kill(p, 0) == 0) {
        ::kill(-p, SIGKILL);
        ::kill(p, SIGKILL);
      }
      for (int retry = 0; retry < 20; ++retry) {
        if (::waitpid(p, nullptr, WNOHANG) > 0 || ::kill(p, 0) < 0) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      std::error_code ec;
      std::filesystem::remove_all(
          "/tmp/virmux_bench_build_" + std::to_string(p), ec);
    }
  }
}

std::vector<BenchmarkTaskConfig> BenchmarkRunner::build_tasks(
    int count, int duration_sec) const {
  const WorkloadType types[] = {
      WorkloadType::BROWSER, WorkloadType::COMPILER, WorkloadType::MODEL,
      WorkloadType::BUILD,   WorkloadType::BROWSER,  WorkloadType::COMPILER,
      WorkloadType::BROWSER, WorkloadType::COMPILER, WorkloadType::MODEL,
      WorkloadType::BUILD,   WorkloadType::BROWSER,  WorkloadType::COMPILER,
  };

  std::vector<BenchmarkTaskConfig> tasks;
  tasks.reserve(std::max(1, count));

  for (int i = 0; i < count; ++i) {
    BenchmarkTaskConfig cfg;
    cfg.type = types[i % 12];
    cfg.duration_seconds = duration_sec;
    cfg.virtual_cores = 2;

    switch (cfg.type) {
      case WorkloadType::BROWSER:
        cfg.name = "browser_" + std::to_string(i);
        cfg.memory_limit_bytes = 1073741824ULL;  // 1 GiB
        cfg.is_heavy = false;
        cfg.target_alloc_mb = quick_ ? 32 : 128;
        break;
      case WorkloadType::COMPILER:
        cfg.name = "compiler_" + std::to_string(i);
        cfg.memory_limit_bytes = 1073741824ULL;  // 1 GiB
        cfg.is_heavy = true;
        cfg.target_alloc_mb = 0;
        break;
      case WorkloadType::MODEL:
        cfg.name = "model_" + std::to_string(i);
        cfg.memory_limit_bytes = 1073741824ULL;  // 1 GiB
        cfg.is_heavy = true;
        cfg.target_alloc_mb = quick_ ? 16 : 64;
        break;
      case WorkloadType::BUILD:
        cfg.name = "build_" + std::to_string(i);
        cfg.memory_limit_bytes = 536870912ULL;  // 512 MiB
        cfg.is_heavy = false;
        cfg.target_alloc_mb = 0;
        break;
    }

    tasks.push_back(std::move(cfg));
  }

  return tasks;
}

BenchmarkResult BenchmarkRunner::execute_suite(
    const std::vector<BenchmarkTaskConfig>& tasks, bool managed,
    int duration_sec) {
  (void)duration_sec;

  BenchmarkResult res;
  res.mode = managed ? "managed" : "baseline";
  res.task_count = static_cast<int>(tasks.size());

  if (stop_flag_.load(std::memory_order_relaxed)) {
    return res;
  }

  {
    std::lock_guard<std::mutex> lock(pids_mutex_);
    active_pids_.clear();
  }

  InputLatencyProbe probe;
  TelemetrySampler telemetry;

  telemetry.start(30);
  probe.start(20);

  struct ProcessCleaner {
    BenchmarkRunner& runner;
    ~ProcessCleaner() { runner.reap_children(); }
  } cleaner{*this};

  std::atomic<int> success_count{0};
  std::atomic<int> failed_count{0};
  std::atomic<int> oom_count{0};

  AdmissionGate gate(2, std::chrono::seconds(10));

  const auto start_time = std::chrono::steady_clock::now();

  std::vector<std::thread> workers;
  workers.reserve(tasks.size());

  for (const auto& task : tasks) {
    workers.emplace_back([this, task, managed, &gate, &telemetry,
                          &success_count, &failed_count, &oom_count]() {
      if (stop_flag_.load(std::memory_order_relaxed)) {
        return;
      }

      std::unique_ptr<ScopedToken> token;
      if (managed && task.is_heavy) {
        while (!stop_flag_.load(std::memory_order_relaxed)) {
          token = std::make_unique<ScopedToken>(gate,
                                                std::chrono::milliseconds(100));
          if (token->acquired()) {
            break;
          }
        }
      }

      if (stop_flag_.load(std::memory_order_relaxed)) {
        return;
      }

      pid_t pid = ::fork();
      if (pid < 0) {
        failed_count++;
        return;
      }

      if (pid == 0) {
        ::setpgid(0, 0);

        if (managed) {
          set_worker_oom_score(0);

          cpu_set_t cpuset;
          CPU_ZERO(&cpuset);
          long ncores = sysconf(_SC_NPROCESSORS_ONLN);
          if (ncores > 2) {
            for (long c = 2; c < ncores; ++c) {
              CPU_SET(c, &cpuset);
            }
            sched_setaffinity(0, sizeof(cpuset), &cpuset);
          } else if (ncores == 2) {
            CPU_SET(1, &cpuset);
            sched_setaffinity(0, sizeof(cpuset), &cpuset);
          }
        }

        int rc = run_synthetic_workload(task.type, task.duration_seconds,
                                        task.target_alloc_mb, nullptr);
        ::_exit(rc);
      }

      {
        std::lock_guard<std::mutex> lock(pids_mutex_);
        if (stop_flag_.load(std::memory_order_relaxed)) {
          ::kill(-pid, SIGKILL);
          ::kill(pid, SIGKILL);
        }
        active_pids_.push_back(pid);
      }
      telemetry.add_target_pid(pid);

      int status = 0;
      pid_t wr = 0;
      while ((wr = ::waitpid(pid, &status, 0)) < 0 && errno == EINTR) {
        if (stop_flag_.load(std::memory_order_relaxed)) {
          break;
        }
      }

      if (wr > 0) {
        std::lock_guard<std::mutex> lock(pids_mutex_);
        auto it = std::find(active_pids_.begin(), active_pids_.end(), pid);
        if (it != active_pids_.end()) {
          *it = -1;
        }
      }

      if (wr > 0) {
        if (WIFEXITED(status)) {
          if (WEXITSTATUS(status) == 0) {
            success_count++;
          } else {
            failed_count++;
          }
        } else if (WIFSIGNALED(status)) {
          failed_count++;
          int sig = WTERMSIG(status);
          if (sig == SIGKILL || sig == SIGSEGV) {
            oom_count++;
          }
        }
      }
    });
  }

  std::atomic<bool> suite_done{false};
  std::thread watcher([this, &suite_done]() {
    while (!suite_done.load(std::memory_order_relaxed)) {
      if (stop_flag_.load(std::memory_order_relaxed)) {
        reap_children();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  for (auto& w : workers) {
    if (w.joinable()) {
      w.join();
    }
  }

  suite_done.store(true, std::memory_order_relaxed);
  if (watcher.joinable()) {
    watcher.join();
  }

  const auto elapsed = std::chrono::duration<double>(
                           std::chrono::steady_clock::now() - start_time)
                           .count();

  probe.stop();
  telemetry.stop();

  res.wall_time_sec = elapsed;
  res.peak_rss_bytes = telemetry.peak_rss_bytes();
  res.peak_swap_bytes = telemetry.peak_swap_bytes();
  res.peak_ksm_bytes_saved = telemetry.peak_ksm_bytes_saved();
  res.avg_latency_ms = probe.avg_latency_ms();
  res.p99_latency_ms = probe.p99_latency_ms();
  res.min_latency_ms = probe.min_latency_ms();
  res.max_latency_ms = probe.max_latency_ms();
  res.latency_samples = probe.sample_count();
  res.oom_killed_count = oom_count.load();
  res.successful_tasks = success_count.load();
  res.failed_tasks = failed_count.load();

  return res;
}

BenchmarkResult BenchmarkRunner::run_baseline(int duration_sec, int tasks) {
  int dur = duration_sec > 0 ? duration_sec : (quick_ ? 1 : 5);
  int tcount = tasks > 0 ? tasks : (quick_ ? 4 : 12);
  auto task_configs = build_tasks(tcount, dur);
  return execute_suite(task_configs, false, dur);
}

BenchmarkResult BenchmarkRunner::run_managed(int duration_sec, int tasks) {
  int dur = duration_sec > 0 ? duration_sec : (quick_ ? 1 : 5);
  int tcount = tasks > 0 ? tasks : (quick_ ? 4 : 12);
  auto task_configs = build_tasks(tcount, dur);
  return execute_suite(task_configs, true, dur);
}

BenchmarkComparison BenchmarkRunner::run_gauntlet(int duration_sec, int tasks) {
  int dur = duration_sec > 0 ? duration_sec : (quick_ ? 1 : 5);
  int tcount = tasks > 0 ? tasks : (quick_ ? 4 : 12);
  BenchmarkComparison comp;
  comp.baseline = run_baseline(dur, tcount);
  if (stop_flag_.load(std::memory_order_relaxed)) {
    return comp;
  }
  comp.managed = run_managed(dur, tcount);
  return comp;
}

}  // namespace virmux
