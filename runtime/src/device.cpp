#include "lacc/device.hpp"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace lacc {

namespace {
template <typename T>
void ioc(int fd, unsigned long req, T* arg, const char* what) {
  if (::ioctl(fd, req, arg) < 0) throw Error(std::string(what) + ": " + std::strerror(errno), errno);
}
}  // namespace

Buffer& Buffer::operator=(Buffer&& o) noexcept {
  if (this != &o) {
    this->~Buffer();
    dev_ = o.dev_;
    offset_ = o.offset_;
    size_ = o.size_;
    map_ = o.map_;
    o.dev_ = nullptr;
    o.map_ = nullptr;
  }
  return *this;
}

Buffer::~Buffer() {
  if (map_) ::munmap(map_, size_);
  map_ = nullptr;
  if (dev_) dev_->release(offset_, size_);
  dev_ = nullptr;
}

void* Buffer::map() {
  if (!map_) {
    void* p = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, dev_->fd(), static_cast<off_t>(offset_));
    if (p == MAP_FAILED) throw Error(std::string("mmap: ") + std::strerror(errno), errno);
    map_ = p;
  }
  return map_;
}

Device::Device(const std::string& path) : fd_(::open(path.c_str(), O_RDWR | O_CLOEXEC)) {
  if (fd_ < 0) throw Error("open " + path + ": " + std::strerror(errno), errno);
}

Device::~Device() {
  if (fd_ >= 0) ::close(fd_);
}

lacc_info Device::info() const {
  lacc_info i{};
  ioc(fd_, LACC_IOC_INFO, &i, "LACC_IOC_INFO");
  return i;
}

Buffer Device::alloc(uint64_t size) {
  lacc_bo bo{};
  bo.size = size;
  ioc(fd_, LACC_IOC_ALLOC, &bo, "LACC_IOC_ALLOC");
  return Buffer(this, bo.offset, bo.size);
}

void Device::release(uint64_t offset, uint64_t size) {
  lacc_bo bo{};
  bo.offset = offset;
  bo.size = size;
  ::ioctl(fd_, LACC_IOC_FREE, &bo);
}

uint64_t Device::write(const Buffer& dst, const void* src, uint64_t size, uint64_t offset, Mode mode) {
  lacc_xfer x{};
  x.user_ptr = reinterpret_cast<uint64_t>(src);
  x.dev_offset = dst.offset() + offset;
  x.size = size;
  x.dir = LACC_DIR_H2D;
  x.mode = static_cast<uint32_t>(mode);
  ioc(fd_, LACC_IOC_XFER, &x, "LACC_IOC_XFER");
  return x.fence;
}

uint64_t Device::read(const Buffer& src, void* dst, uint64_t size, uint64_t offset, Mode mode) {
  lacc_xfer x{};
  x.user_ptr = reinterpret_cast<uint64_t>(dst);
  x.dev_offset = src.offset() + offset;
  x.size = size;
  x.dir = LACC_DIR_D2H;
  x.mode = static_cast<uint32_t>(mode);
  ioc(fd_, LACC_IOC_XFER, &x, "LACC_IOC_XFER");
  return x.fence;
}

uint64_t Device::fill(const Buffer& dst, uint32_t value, uint64_t size, uint64_t offset) {
  lacc_fill f{};
  f.dev_offset = dst.offset() + offset;
  f.size = size;
  f.value = value;
  ioc(fd_, LACC_IOC_FILL, &f, "LACC_IOC_FILL");
  return f.fence;
}

uint64_t Device::gemm(const Buffer& a, const Buffer& b, const Buffer& c, uint32_t m, uint32_t n, uint32_t k,
                      Kind kind) {
  lacc_gemm g{};
  g.a = a.offset();
  g.b = b.offset();
  g.c = c.offset();
  g.m = m;
  g.n = n;
  g.k = k;
  g.kind = static_cast<uint32_t>(kind);
  ioc(fd_, LACC_IOC_GEMM, &g, "LACC_IOC_GEMM");
  return g.fence;
}

WaitResult Device::wait(uint64_t fence, uint32_t timeout_ms) {
  lacc_wait w{};
  w.fence = fence;
  w.timeout_ms = timeout_ms;
  ioc(fd_, LACC_IOC_WAIT, &w, "LACC_IOC_WAIT");
  return WaitResult{w.result, w.status, w.submit_ns, w.done_ns};
}

WaitResult Device::sync(uint64_t fence) {
  WaitResult r = wait(fence);
  if (!r.ok())
    throw Error("job " + std::to_string(fence) + " failed: result " + std::to_string(r.result) + ", status " +
                    std::to_string(r.status),
                EIO);
  return r;
}

}  // namespace lacc
