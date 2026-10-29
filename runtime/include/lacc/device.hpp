// A small C++ runtime over /dev/laccN: buffer objects, transfers, GEMMs and fences.
#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

extern "C" {
#include "uapi/lacc.h"
}

namespace lacc {

class Error : public std::runtime_error {
 public:
  Error(const std::string& what, int err) : std::runtime_error(what), err_(err) {}
  int code() const { return err_; }

 private:
  int err_;
};

enum class Mode : uint32_t { ScatterGather = LACC_XFER_SG, PerSegment = LACC_XFER_PER_SEG };
enum class Kind : uint32_t { Lns8 = LACC_GEMM_LNS8, Int8 = LACC_GEMM_I8 };

struct WaitResult {
  uint32_t result = 0;   // LACC_WAIT_*
  uint32_t status = 0;   // LACC_CQE_STATUS_*
  uint64_t submit_ns = 0, done_ns = 0;
  bool ok() const { return result == LACC_WAIT_OK; }
  uint64_t device_ns() const { return done_ns > submit_ns ? done_ns - submit_ns : 0; }
};

class Device;

// Device memory owned by one file descriptor; freed on destruction.
class Buffer {
 public:
  Buffer() = default;
  Buffer(Device* dev, uint64_t offset, uint64_t size) : dev_(dev), offset_(offset), size_(size) {}
  Buffer(Buffer&& o) noexcept { *this = std::move(o); }
  Buffer& operator=(Buffer&& o) noexcept;
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  ~Buffer();

  uint64_t offset() const { return offset_; }
  uint64_t size() const { return size_; }
  // Map the buffer's device memory (BAR2) into this process, write-combined.
  void* map();

 private:
  Device* dev_ = nullptr;
  uint64_t offset_ = 0, size_ = 0;
  void* map_ = nullptr;
};

class Device {
 public:
  explicit Device(const std::string& path = "/dev/lacc0");
  ~Device();
  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  lacc_info info() const;
  Buffer alloc(uint64_t size);
  uint64_t write(const Buffer& dst, const void* src, uint64_t size, uint64_t offset = 0,
                 Mode mode = Mode::ScatterGather);
  uint64_t read(const Buffer& src, void* dst, uint64_t size, uint64_t offset = 0, Mode mode = Mode::ScatterGather);
  uint64_t fill(const Buffer& dst, uint32_t value, uint64_t size, uint64_t offset = 0);
  uint64_t gemm(const Buffer& a, const Buffer& b, const Buffer& c, uint32_t m, uint32_t n, uint32_t k, Kind kind);
  WaitResult wait(uint64_t fence, uint32_t timeout_ms = 10000);
  // convenience: submit and wait, throwing unless the job succeeded
  WaitResult sync(uint64_t fence);

  int fd() const { return fd_; }
  void release(uint64_t offset, uint64_t size);

 private:
  int fd_ = -1;
};

}  // namespace lacc
