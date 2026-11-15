/**
 * @file bench.cpp
 * @brief Implementation of synthetic benchmark workloads, input latency probe, and telemetry sampler.
 */

#include "virmux/bench.hpp"

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <stdexcept>

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
    lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
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
                       int duration_seconds, const std::atomic<bool>* stop_flag) {
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
      const std::string node_prefix =
          "<div class=\"dom-node\" id=\"node_" + std::to_string(i) +
          "\"><span>Content block for virtual DOM tree layout element</span></div>\n";
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
          "(int a, int b) { int sum = 0; for (int i = 0; i < 100; ++i) sum += (a * i + b) ^ 0x5a; return sum; }";

      while (!check_should_stop(start_time, duration_seconds, stop_flag)) {
        // Token parsing and symbol hashing
        for (char c : synthetic_code) {
          hash_accumulator ^= static_cast<uint64_t>(c);
          hash_accumulator *= 0x100000001b3ULL;
        }

        // CPU burst: integer and floating point math loop
        double math_burst = 1.0;
        for (int k = 0; k < 2000; ++k) {
          math_burst = std::sin(math_burst + static_cast<double>(k) * 0.001) * 1.0001;
        }
        volatile double dummy = math_burst + static_cast<double>(hash_accumulator);
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
          ? std::min<size_t>(target_alloc_mb * 1024ULL * 1024ULL, 64ULL * 1024ULL * 1024ULL)
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
    } catch (const std::bad_alloc&) {}

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

  char tmpl[] = "/tmp/virmux_bench_build_XXXXXX";
  char* temp_dir_c = ::mkdtemp(tmpl);
  if (!temp_dir_c) {
    return -1;
  }
  const std::filesystem::path temp_dir(temp_dir_c);

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
      return run_compiler_workload(duration_seconds, target_alloc_mb, stop_flag);
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
  std::scoped_lock lock(samples_mutex_, other.samples_mutex_, io_mutex_, other.io_mutex_);
  force_pipe_fallback_ = other.force_pipe_fallback_;
  is_pty_ = other.is_pty_;
  master_fd_ = other.master_fd_;
  slave_fd_ = other.slave_fd_;
  samples_ = std::move(other.samples_);
  other.master_fd_ = -1;
  other.slave_fd_ = -1;
}

InputLatencyProbe& InputLatencyProbe::operator=(InputLatencyProbe&& other) noexcept {
  if (this != &other) {
    stop();
    close_descriptors();
    other.stop();
    std::scoped_lock lock(samples_mutex_, other.samples_mutex_, io_mutex_, other.io_mutex_);
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
  while (::read(read_fd, drain_buf, sizeof(drain_buf)) > 0) {}
  if (is_pty_) {
    while (::read(slave_fd_, drain_buf, sizeof(drain_buf)) > 0) {}
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

  int ret = ::poll(&pfd, 1, 1000);  // 1000ms timeout
  if (ret > 0 && (pfd.revents & POLLIN)) {
    char resp = 0;
    ssize_t nr = ::read(read_fd, &resp, 1);
    if (nr > 0) {
      const auto t1 = std::chrono::steady_clock::now();
      const double latency_ms =
          std::chrono::duration<double, std::milli>(t1 - t0).count();

      if (is_pty_) {
        while (::read(slave_fd_, drain_buf, sizeof(drain_buf)) > 0) {}
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

bool InputLatencyProbe::is_running() const {
  return running_.load();
}

bool InputLatencyProbe::is_pty() const {
  return is_pty_;
}

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

TelemetrySampler::~TelemetrySampler() {
  stop();
}

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

TelemetrySampler& TelemetrySampler::operator=(TelemetrySampler&& other) noexcept {
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

bool TelemetrySampler::is_running() const {
  return running_.load();
}

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

}  // namespace virmux
