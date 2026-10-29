// lacc-validate: the bring-up and validation suite, run inside the guest.
//
// Each check prints one JSON line prefixed "@@R " on stdout: name, group, pass, detail and
// metrics. The host collects the lines from the serial console into the bring-up report.
// Checks go from the bottom up: PCI enumeration, the driver's interfaces, DMA, compute,
// board telemetry, power management, fault handling, concurrency and performance.

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "lacc/device.hpp"
#include "lacc/lns.hpp"

namespace {

using Clock = std::chrono::steady_clock;
using Metrics = std::map<std::string, double>;
using R = std::pair<bool, std::string>;   // a check's verdict and detail

std::string g_sys;        // /sys/bus/pci/devices/<bdf>
std::string g_attr;       // <g_sys>/lacc
int g_failed = 0;

std::string json_escape(const std::string& s) {
  std::string o;
  for (char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\t': o += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) o += ' ';
        else o += c;
    }
  }
  return o;
}

void emit(const std::string& group, const std::string& name, bool pass, const std::string& detail,
          const Metrics& m = {}) {
  std::ostringstream os;
  os << "@@R {\"group\":\"" << group << "\",\"name\":\"" << json_escape(name) << "\",\"pass\":" << (pass ? "true" : "false")
     << ",\"detail\":\"" << json_escape(detail) << "\",\"metrics\":{";
  bool first = true;
  for (auto& [k, v] : m) {
    if (!first) os << ",";
    first = false;
    os << "\"" << k << "\":";
    if (std::isfinite(v)) os << std::setprecision(12) << v;
    else os << "null";
  }
  os << "}}";
  std::cout << os.str() << std::endl;
  if (!pass) ++g_failed;
}

// Run one check; an exception is a failure with its message.
void check(const std::string& group, const std::string& name, const std::function<std::pair<bool, std::string>(Metrics&)>& fn) {
  Metrics m;
  try {
    auto [ok, detail] = fn(m);
    emit(group, name, ok, detail, m);
  } catch (const std::exception& e) {
    emit(group, name, false, std::string("exception: ") + e.what(), m);
  }
}

std::string slurp(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  std::string s = ss.str();
  while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
  return s;
}

bool spit(const std::string& path, const std::string& v) {
  std::ofstream f(path);
  f << v;
  f.flush();
  return static_cast<bool>(f);
}

long attr_l(const std::string& name) { return std::stol(slurp(g_attr + "/" + name), nullptr, 0); }

std::map<std::string, long> stats() {
  std::map<std::string, long> m;
  std::istringstream is(slurp(g_attr + "/stats"));
  std::string k;
  long v;
  while (is >> k >> v) m[k] = v;
  return m;
}

std::string find_card() {
  DIR* d = opendir("/sys/bus/pci/devices");
  if (!d) return {};
  std::string found;
  while (auto* e = readdir(d)) {
    std::string p = std::string("/sys/bus/pci/devices/") + e->d_name;
    if (slurp(p + "/vendor") == "0x1234" && slurp(p + "/device") == "0x7acc") found = p;
  }
  closedir(d);
  return found;
}

std::string find_hwmon(const std::string& name) {
  DIR* d = opendir("/sys/class/hwmon");
  if (!d) return {};
  std::string found;
  while (auto* e = readdir(d)) {
    std::string p = std::string("/sys/class/hwmon/") + e->d_name;
    if (e->d_name[0] != '.' && slurp(p + "/name") == name) found = p;
  }
  closedir(d);
  return found;
}

double ms_since(Clock::time_point t0) {
  return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

std::vector<uint8_t> pattern(size_t n, uint32_t seed) {
  std::vector<uint8_t> v(n);
  std::mt19937 rng(seed);
  for (auto& b : v) b = static_cast<uint8_t>(rng());
  return v;
}

// ---- checks ----------------------------------------------------------------------------------

void pci_checks() {
  check("pci", "card enumerates as 1234:7acc, class 0x1200", [](Metrics&) {
    g_sys = find_card();
    if (g_sys.empty()) return R{false, std::string("no 1234:7acc device on the bus")};
    g_attr = g_sys + "/lacc";
    return R{slurp(g_sys + "/class") == "0x120000", g_sys.substr(g_sys.rfind('/') + 1) + " class " + slurp(g_sys + "/class")};
  });
  check("pci", "BARs: 64 KiB registers, 64 MiB device memory, MSI-X table", [](Metrics& m) {
    std::istringstream is(slurp(g_sys + "/resource"));
    std::vector<std::pair<unsigned long long, unsigned long long>> bars;
    unsigned long long s, e, f;
    while (is >> std::hex >> s >> e >> f) bars.emplace_back(s, e);
    auto size = [&](size_t i) { return bars.size() > i && bars[i].first ? bars[i].second - bars[i].first + 1 : 0ull; };
    m["bar0_bytes"] = static_cast<double>(size(0));
    m["bar2_bytes"] = static_cast<double>(size(2));
    m["bar4_bytes"] = static_cast<double>(size(4));
    return R{size(0) == 0x10000 && size(2) == (64ull << 20) && size(4) > 0,
                     "BAR0 " + std::to_string(size(0)) + ", BAR2 " + std::to_string(size(2))};
  });
  check("pci", "PCIe endpoint: link trained, AER present", [](Metrics& m) {
    std::ifstream f(g_sys + "/config", std::ios::binary);
    std::vector<uint8_t> cfg((std::istreambuf_iterator<char>(f)), {});
    if (cfg.size() < 0x104) return R{false, std::string("config space shorter than 256 bytes")};
    int ptr = cfg[0x34], exp = 0, guard = 0;
    while (ptr && guard++ < 48) {
      if (cfg[ptr] == 0x10) exp = ptr;
      ptr = cfg[ptr + 1];
    }
    uint32_t ext = cfg[0x100] | cfg[0x101] << 8;
    m["pcie_cap_offset"] = exp;
    if (!exp) return R{false, std::string("no PCI Express capability")};
    const int type = (cfg[exp + 2] >> 4) & 0xf;                // 0 = endpoint
    const uint16_t lnksta = cfg[exp + 0x12] | cfg[exp + 0x13] << 8;
    const int speed = lnksta & 0xf, width = (lnksta >> 4) & 0x3f;
    static const char* gts[] = {"?", "2.5", "5", "8", "16", "32", "64"};
    m["link_speed_code"] = speed;
    m["link_width"] = width;
    char b[200];
    std::snprintf(b, sizeof(b), "express endpoint (type %d) at %#x, link up at %s GT/s x%d%s", type, exp,
                  gts[speed < 7 ? speed : 0], width, ext == 1 ? ", AER at 0x100" : ", no AER");
    return R{type == 0 && width > 0 && ext == 0x0001, std::string(b)};
  });
  check("pci", "MSI-X: two vectors allocated", [](Metrics& m) {
    int n = 0;
    if (DIR* d = opendir((g_sys + "/msi_irqs").c_str())) {
      while (auto* e = readdir(d)) n += e->d_name[0] != '.';
      closedir(d);
    }
    m["vectors"] = n;
    return R{n == 2, std::to_string(n) + " vectors"};
  });
}

void driver_checks(lacc::Device& dev) {
  check("driver", "ioctl INFO matches the register map", [&](Metrics& m) {
    auto i = dev.info();
    m["devmem_mib"] = static_cast<double>(i.devmem_size >> 20);
    bool ok = i.hw_version == LACC_HW_VERSION && (i.caps & LACC_CAP_GEMM_LNS8) && (i.status & LACC_STATUS_READY) &&
              i.devmem_size == (64ull << 20);
    char b[160];
    std::snprintf(b, sizeof(b), "hw %#x caps %#x status %#x fw %u clock %u MHz", i.hw_version, i.caps, i.status,
                  i.fw_version, i.clock_mhz);
    return R{ok, std::string(b)};
  });
  check("driver", "debugfs register dump reads the ID", [](Metrics&) {
    std::string regs = slurp("/sys/kernel/debug/lacc0/regs");
    return R{regs.find("0x4c414343") != std::string::npos, regs.substr(0, regs.find('\n'))};
  });
  check("driver", "device memory allocator: fill, reuse, exhaustion", [&](Metrics& m) {
    std::vector<lacc::Buffer> bufs;
    uint64_t total = 0;
    try {
      for (;;) {
        bufs.push_back(dev.alloc(8 << 20));
        total += 8 << 20;
      }
    } catch (const lacc::Error& e) {
      if (e.code() != ENOSPC) throw;
    }
    m["allocated_mib"] = static_cast<double>(total >> 20);
    bufs.clear();
    auto again = dev.alloc(60 << 20);    // freed extents must merge back
    return R{total == (64ull << 20), "allocated " + std::to_string(total >> 20) + " MiB, then 60 MiB again"};
  });
}

void dma_checks(lacc::Device& dev) {
  const size_t sizes[] = {4, 100, 4096, 4097, 65536, 1u << 20, 9u << 20};
  for (auto mode : {lacc::Mode::ScatterGather, lacc::Mode::PerSegment}) {
    const char* mname = mode == lacc::Mode::ScatterGather ? "scatter-gather" : "per-segment";
    check("dma", std::string("host -> card -> host round trip, ") + mname, [&](Metrics& m) {
      auto buf = dev.alloc(16 << 20);
      size_t done = 0;
      for (size_t sz : sizes) {
        for (size_t mis : {0, 1, 7}) {   // misaligned user pointers cross page boundaries oddly
          auto src = pattern(sz + 8, static_cast<uint32_t>(sz + mis));
          std::vector<uint8_t> dst(sz + 8, 0);
          dev.sync(dev.write(buf, src.data() + mis, sz, 0, mode));
          dev.sync(dev.read(buf, dst.data() + mis, sz, 0, mode));
          if (std::memcmp(src.data() + mis, dst.data() + mis, sz))
            return R{false, "mismatch at size " + std::to_string(sz) + " offset " + std::to_string(mis)};
          ++done;
        }
      }
      m["round_trips"] = static_cast<double>(done);
      return R{true, std::to_string(done) + " round trips, 4 B to 9 MiB, aligned and misaligned"};
    });
  }
  check("dma", "fill engine and mmap of device memory agree", [&](Metrics&) {
    auto buf = dev.alloc(1 << 20);
    dev.sync(dev.fill(buf, 0xA5C3F00Du, 1 << 20));
    auto* p = static_cast<volatile uint32_t*>(buf.map());
    for (size_t i = 0; i < (1 << 18); i += 997)
      if (p[i] != 0xA5C3F00Du) return R{false, "mmap read " + std::to_string(p[i]) + " at word " + std::to_string(i)};
    p[5] = 0x12345678;                     // write through the BAR, read back by DMA
    std::vector<uint32_t> back(16);
    dev.sync(dev.read(buf, back.data(), back.size() * 4));
    return R{back[5] == 0x12345678 && back[4] == 0xA5C3F00Du, "BAR2 mapping and DMA see the same memory"};
  });
  check("dma", "out-of-range and foreign buffers are refused", [&](Metrics&) {
    auto buf = dev.alloc(4096);
    std::vector<uint8_t> big(8192);
    bool refused = false;
    try {
      dev.write(buf, big.data(), big.size());
    } catch (const lacc::Error& e) {
      refused = e.code() == EACCES;
    }
    return R{refused, refused ? "EACCES for a transfer past the end of the buffer" : "transfer was accepted"};
  });
}

struct GemmCase {
  uint32_t m, n, k;
};

void gemm_checks(lacc::Device& dev) {
  const GemmCase cases[] = {{1, 1, 1}, {7, 5, 3}, {64, 64, 64}, {128, 256, 96}, {257, 129, 63}, {320, 288, 256}};
  check("compute", "LNS8 GEMM equals the reference bit for bit", [&](Metrics& m) {
    std::mt19937 rng(7);
    std::normal_distribution<float> nd(0.f, 1.f);
    double worst_rel = 0;
    for (auto c : cases) {
      std::vector<float> af(c.m * c.k), bf(c.k * c.n);
      for (auto& x : af) x = nd(rng);
      for (auto& x : bf) x = nd(rng);
      auto a = lacc::quantize_lns8(af, 1.f);
      auto b = lacc::quantize_lns8(bf, 1.f);
      auto ref = lacc::gemm_lns8_ref(a, b, c.m, c.n, c.k);
      auto A = dev.alloc(a.size()), B = dev.alloc(b.size()), C = dev.alloc(ref.size() * 4);
      dev.sync(dev.write(A, a.data(), a.size()));
      dev.sync(dev.write(B, b.data(), b.size()));
      dev.sync(dev.gemm(A, B, C, c.m, c.n, c.k, lacc::Kind::Lns8));
      std::vector<int32_t> got(ref.size());
      dev.sync(dev.read(C, got.data(), got.size() * 4));
      if (got != ref) return R{false, "mismatch for " + std::to_string(c.m) + "x" + std::to_string(c.n) + "x" + std::to_string(c.k)};
      // the datapath's own error: against exact products of the same LNS8 inputs
      std::vector<double> da(a.size()), db(b.size());
      for (size_t i = 0; i < a.size(); ++i) da[i] = lacc::from_lns8(a[i]);
      for (size_t i = 0; i < b.size(); ++i) db[i] = lacc::from_lns8(b[i]);
      double num = 0, den = 0;
      for (uint32_t i = 0; i < c.m; ++i)
        for (uint32_t j = 0; j < c.n; ++j) {
          double fp = 0;
          for (uint32_t p = 0; p < c.k; ++p) fp += da[i * c.k + p] * db[p * c.n + j];
          double d = got[i * c.n + j] / 256.0 - fp;
          num += d * d;
          den += fp * fp;
        }
      worst_rel = std::max(worst_rel, std::sqrt(num / std::max(den, 1e-30)));
    }
    m["worst_relative_rms_error"] = worst_rel;
    return R{worst_rel < 0.01, "6 shapes up to 320x288x256 identical; datapath vs exact products of the same inputs: " +
                                           std::to_string(worst_rel * 100).substr(0, 5) + "% RMS"};
  });
  check("compute", "INT8 GEMM equals the reference", [&](Metrics&) {
    std::mt19937 rng(11);
    for (auto c : cases) {
      std::vector<int8_t> a(c.m * c.k), b(c.k * c.n);
      for (auto& x : a) x = static_cast<int8_t>(rng() % 255 - 127);
      for (auto& x : b) x = static_cast<int8_t>(rng() % 255 - 127);
      auto ref = lacc::gemm_i8_ref(a, b, c.m, c.n, c.k);
      auto A = dev.alloc(a.size()), B = dev.alloc(b.size()), C = dev.alloc(ref.size() * 4);
      dev.sync(dev.write(A, a.data(), a.size()));
      dev.sync(dev.write(B, b.data(), b.size()));
      dev.sync(dev.gemm(A, B, C, c.m, c.n, c.k, lacc::Kind::Int8));
      std::vector<int32_t> got(ref.size());
      dev.sync(dev.read(C, got.data(), got.size() * 4));
      if (got != ref) return R{false, "mismatch for " + std::to_string(c.m) + "x" + std::to_string(c.n) + "x" + std::to_string(c.k)};
    }
    return R{true, std::string("6 shapes identical")};
  });
  check("compute", "LNS8 vs INT8 on the card: matmul error by data shape, and modelled energy", [&](Metrics& m) {
    const uint32_t M = 192, N = 192, K = 256;
    auto A = dev.alloc(M * K), B = dev.alloc(K * N), C = dev.alloc(M * N * 4);
    auto energy = [&] { return std::stod(slurp(g_attr + "/active_energy_uj")); };
    struct Out {
      double err_lns, err_i8, e_lns, e_i8;
    };
    // per-tensor scales from the data, as a runtime would choose them
    auto compare = [&](const std::vector<float>& af, const std::vector<float>& bf) {
      std::vector<double> fp(M * N, 0);
      for (uint32_t i = 0; i < M; ++i)
        for (uint32_t p = 0; p < K; ++p)
          for (uint32_t j = 0; j < N; ++j) fp[i * N + j] += double(af[i * K + p]) * bf[p * N + j];
      auto rel = [&](const std::vector<int32_t>& c, double scale) {
        double num = 0, den = 0;
        for (size_t i = 0; i < c.size(); ++i) {
          double d = c[i] * scale - fp[i];
          num += d * d, den += fp[i] * fp[i];
        }
        return std::sqrt(num / den);
      };
      auto amax = [](const std::vector<float>& x) {
        float a = 0;
        for (float v : x) a = std::max(a, std::fabs(v));
        return a;
      };
      const float la = amax(af) / 128, lb = amax(bf) / 128, ia = amax(af) / 127, ib = amax(bf) / 127;
      auto run = [&](const auto& a, const auto& b, lacc::Kind kind, double& e) {
        dev.sync(dev.write(A, a.data(), a.size()));
        dev.sync(dev.write(B, b.data(), b.size()));
        double e0 = energy();
        for (int r = 0; r < 10; ++r) dev.sync(dev.gemm(A, B, C, M, N, K, kind));
        e = (energy() - e0) / 10;
        std::vector<int32_t> c(M * N);
        dev.sync(dev.read(C, c.data(), c.size() * 4));
        return c;
      };
      Out o{};
      auto cl = run(lacc::quantize_lns8(af, la), lacc::quantize_lns8(bf, lb), lacc::Kind::Lns8, o.e_lns);
      auto ci = run(lacc::quantize_i8(af, ia), lacc::quantize_i8(bf, ib), lacc::Kind::Int8, o.e_i8);
      o.err_lns = rel(cl, double(la) * lb / 256);
      o.err_i8 = rel(ci, double(ia) * ib);
      return o;
    };
    std::mt19937 rng(3);
    std::normal_distribution<float> nd(0.f, 1.f);
    std::vector<float> ga(M * K), gb(K * N), ha(M * K), hb(K * N);
    for (auto& x : ga) x = nd(rng);
    for (auto& x : gb) x = nd(rng);
    // heavy-tailed: a Gaussian times a log-normal scale, like activations with outlier channels
    for (auto& x : ha) x = nd(rng) * std::exp(1.5f * nd(rng));
    for (auto& x : hb) x = nd(rng) * std::exp(1.5f * nd(rng));
    auto g = compare(ga, gb), h = compare(ha, hb);
    m["gaussian_lns8_rel_error"] = g.err_lns;
    m["gaussian_int8_rel_error"] = g.err_i8;
    m["heavy_tail_lns8_rel_error"] = h.err_lns;
    m["heavy_tail_int8_rel_error"] = h.err_i8;
    m["lns8_energy_uj_per_gemm"] = g.e_lns;
    m["int8_energy_uj_per_gemm"] = g.e_i8;
    char b[300];
    std::snprintf(b, sizeof(b),
                  "error vs FP64, Gaussian: LNS8 %.2f%%, INT8 %.2f%%; heavy-tailed: LNS8 %.2f%%, INT8 %.2f%%; "
                  "energy per GEMM LNS8 %.1f uJ, INT8 %.1f uJ (modelled)",
                  g.err_lns * 100, g.err_i8 * 100, h.err_lns * 100, h.err_i8 * 100, g.e_lns, g.e_i8);
    return R{g.err_lns < 0.06 && g.err_i8 < 0.06 && h.err_lns < 0.1 && g.e_lns < g.e_i8, std::string(b)};
  });
}

void board_checks() {
  check("board", "PMBus regulator on the card's SMBus: hwmon sensors in range", [](Metrics& m) {
    std::string h = find_hwmon("pmbus");
    if (h.empty()) return R{false, std::string("no pmbus hwmon device")};
    double vin = std::stod(slurp(h + "/in1_input")) / 1000, vout = std::stod(slurp(h + "/in2_input")) / 1000;
    double temp = std::stod(slurp(h + "/temp1_input")) / 1000, pout = std::stod(slurp(h + "/power2_input")) / 1e6;
    m["vin_v"] = vin;
    m["vout_v"] = vout;
    m["temp_c"] = temp;
    m["pout_w"] = pout;
    char b[160];
    std::snprintf(b, sizeof(b), "VIN %.2f V, VOUT %.3f V, %.1f C, POUT %.1f W", vin, vout, temp, pout);
    return R{vin > 11.5 && vin < 12.5 && vout > 0.5 && vout < 1.0 && temp > 20 && temp < 110, std::string(b)};
  });
  check("board", "FRU EEPROM: record and checksum", [](Metrics&) {
    std::string path;
    if (DIR* d = opendir("/sys/bus/i2c/devices")) {
      while (auto* e = readdir(d))
        if (std::string(e->d_name).find("-0050") != std::string::npos) path = std::string("/sys/bus/i2c/devices/") + e->d_name + "/eeprom";
      closedir(d);
    }
    std::ifstream f(path, std::ios::binary);
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(f)), {});
    if (rom.size() != 256) return R{false, "read " + std::to_string(rom.size()) + " bytes from " + path};
    uint8_t sum = 0;
    for (auto b : rom) sum += b;
    std::string text(reinterpret_cast<char*>(rom.data()));
    return R{sum == 0 && text.rfind("LFRU1;", 0) == 0, text + (sum ? " (bad checksum)" : "")};
  });
  check("board", "FRU EEPROM is write-protected", [](Metrics&) {
    std::string path;
    if (DIR* d = opendir("/sys/bus/i2c/devices")) {
      while (auto* e = readdir(d))
        if (std::string(e->d_name).find("-0050") != std::string::npos) path = std::string("/sys/bus/i2c/devices/") + e->d_name + "/eeprom";
      closedir(d);
    }
    int fd = ::open(path.c_str(), O_WRONLY);
    ssize_t w = fd >= 0 ? ::pwrite(fd, "X", 1, 0) : -1;
    if (fd >= 0) ::close(fd);
    return R{w < 0, w < 0 ? "write refused by the board" : "write was accepted"};
  });
  check("board", "SPI NOR as MTD: firmware partitions present", [](Metrics&) {
    std::string mtd = slurp("/proc/mtd");
    bool ok = mtd.find("\"lacc-fw-a\"") != std::string::npos && mtd.find("\"lacc-fw-b\"") != std::string::npos &&
              mtd.find("\"lacc-config\"") != std::string::npos;
    return R{ok, std::string("lacc-fw-a, lacc-fw-b, lacc-config")};
  });
  check("board", "boot ROM verified both slots and booted one", [](Metrics& m) {
    long st = attr_l("fw_status");
    m["fw_version"] = static_cast<double>(attr_l("fw_version"));
    bool ok = (st & LACC_FW_LUT_LOADED) && (st & (LACC_FW_SLOT_A | LACC_FW_SLOT_B));
    return R{ok, "status " + slurp(g_attr + "/fw_status") + ", slot " + slurp(g_attr + "/booted_slot") + ", version " + slurp(g_attr + "/fw_version")};
  });
}

void power_checks(lacc::Device& dev) {
  check("power", "a power cap holds under load and lifts afterwards", [&](Metrics& m) {
    const uint32_t S = 256;
    auto A = dev.alloc(S * S), B = dev.alloc(S * S), C = dev.alloc(S * S * 4);
    std::vector<int8_t> a(S * S, 3);
    dev.sync(dev.write(A, a.data(), a.size()));
    dev.sync(dev.write(B, a.data(), a.size()));
    auto load = [&](int ms) {
      double peak = 0, sum = 0;
      int n = 0;
      auto t0 = Clock::now();
      // keep 16 jobs queued at all times, so the card never runs dry between our reads
      std::deque<uint64_t> inflight;
      while (ms_since(t0) < ms) {
        while (inflight.size() < 16) inflight.push_back(dev.gemm(A, B, C, S, S, S, lacc::Kind::Int8));
        for (int i = 0; i < 4; ++i) {
          dev.sync(inflight.front());
          inflight.pop_front();
        }
        double p = attr_l("power_mw") / 1000.0;
        peak = std::max(peak, p);
        sum += p;
        ++n;
      }
      for (auto x : inflight) dev.sync(x);
      return std::pair{peak, n ? sum / n : 0};
    };
    auto [free_peak, free_avg] = load(800);
    long clock_free = attr_l("clock_mhz");
    spit(g_attr + "/power_cap_mw", "40000");
    load(1500);                              // the controller steps 100 MHz per 10 ms tick
    auto [cap_peak, cap_avg] = load(1000);
    long clock_capped = attr_l("clock_mhz");
    long throttled = attr_l("throttled");
    spit(g_attr + "/power_cap_mw", "0");
    load(1500);
    long clock_after = attr_l("clock_mhz");
    m["capped_peak_w"] = cap_peak;
    m["uncapped_avg_w"] = free_avg;
    m["capped_avg_w"] = cap_avg;
    m["clock_uncapped_mhz"] = clock_free;
    m["clock_capped_mhz"] = clock_capped;
    char b[200];
    std::snprintf(b, sizeof(b), "uncapped %.1f W at %ld MHz; cap 40 W: %.1f W at %ld MHz (throttled %ld); after: %ld MHz",
                  free_avg, clock_free, cap_avg, clock_capped, throttled, clock_after);
    return R{free_avg > 44.0 && cap_avg <= 42.0 && clock_capped < clock_free && clock_after > clock_capped, std::string(b)};
  });
  check("power", "energy counters: board energy and the energy of the work", [&](Metrics& m) {
    auto rd = [](const char* f) { return std::stod(slurp(g_attr + "/" + f)); };
    double e0 = rd("energy_uj"), a0 = rd("active_energy_uj");
    auto t0 = Clock::now();
    auto buf = dev.alloc(4 << 20);
    for (int i = 0; i < 20; ++i) dev.sync(dev.fill(buf, i, 4 << 20));
    double s = ms_since(t0) / 1e3, board = rd("energy_uj") - e0, active = rd("active_energy_uj") - a0;
    m["board_uj"] = board;
    m["active_uj"] = active;
    m["seconds"] = s;
    char b[220];
    // 80 MiB at the modelled 60 pJ/byte is about 5 mJ of active energy
    std::snprintf(b, sizeof(b), "80 MiB of fills: active %.0f uJ (model ~5000); board %.0f uJ over %.2f s, %.1f W average",
                  active, board, s, board / s / 1e6);
    return R{active > 4000 && active < 6000 && board > active, std::string(b)};
  });
}

void fault_checks(lacc::Device& dev) {
  auto inject = [](uint32_t bits) { return spit("/sys/kernel/debug/lacc0/fault_inject", std::to_string(bits)); };
  check("faults", "an injected DMA error fails that job only", [&](Metrics&) {
    auto buf = dev.alloc(4096);
    inject(LACC_FAULT_DMA_ERROR);
    auto r = dev.wait(dev.fill(buf, 1, 4096));
    auto r2 = dev.wait(dev.fill(buf, 2, 4096));
    return R{r.result == LACC_WAIT_FAILED && r.status == LACC_CQE_STATUS_DMA_ERROR && r2.ok(),
                     "first job status " + std::to_string(r.status) + ", next job " + (r2.ok() ? "ok" : "failed")};
  });
  check("faults", "a hung engine is detected, reset and the job aborted", [&](Metrics& m) {
    spit(g_attr + "/hang_timeout_ms", "500");
    auto buf = dev.alloc(4096);
    long resets0 = stats()["resets"];
    inject(LACC_FAULT_HANG);
    auto t0 = Clock::now();
    auto r = dev.wait(dev.fill(buf, 1, 4096), 5000);
    double ms = ms_since(t0);
    auto r2 = dev.wait(dev.fill(buf, 2, 4096));
    m["detect_ms"] = ms;
    spit(g_attr + "/hang_timeout_ms", "2000");
    return R{r.result == LACC_WAIT_ABORTED && r2.ok() && stats()["resets"] == resets0 + 1,
             "engine reported HALTED; watchdog reset the card and aborted the job after " +
                 std::to_string(static_cast<int>(ms)) + " ms; the next job ran"};
  });
  check("faults", "link down: registers read all-ones, the driver waits, resets and recovers", [&](Metrics& m) {
    auto buf = dev.alloc(4096);
    long downs0 = stats()["link_downs"], resets0 = stats()["resets"];
    // read the ID register straight from BAR0, the way the driver sees it
    int fd = ::open((g_sys + "/resource0").c_str(), O_RDONLY | O_SYNC);
    void* bar = fd >= 0 ? ::mmap(nullptr, 4096, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
    auto id = [&] { return bar == MAP_FAILED ? 0u : *static_cast<volatile uint32_t*>(bar); };
    uint32_t before = id();
    inject(LACC_FAULT_LINK_DOWN);
    uint32_t during = id();
    auto t0 = Clock::now();
    // until recovery, submissions either queue or are aborted; afterwards they succeed
    for (int i = 0; i < 100; ++i) {
      try {
        if (dev.wait(dev.fill(buf, i, 4096), 3000).ok() && stats()["resets"] > resets0 && stats()["link_downs"] > downs0) break;
      } catch (const lacc::Error&) {
      }
      usleep(50000);
    }
    double ms = ms_since(t0);
    m["recovery_ms"] = ms;
    uint32_t after = id();
    if (bar != MAP_FAILED) ::munmap(bar, 4096);
    if (fd >= 0) ::close(fd);
    long downs = stats()["link_downs"] - downs0, resets = stats()["resets"] - resets0;
    bool ok = before == LACC_ID_MAGIC && during == 0xffffffffu && after == LACC_ID_MAGIC && downs == 1 &&
              resets == 1 && dev.wait(dev.fill(buf, 9, 4096)).ok();
    char b[200];
    std::snprintf(b, sizeof(b),
                  "ID read %#x, %#x while down, %#x after; %ld outage counted, %ld reset; jobs run again after %.0f ms",
                  before, during, after, downs, resets, ms);
    return R{ok, std::string(b)};
  });
  check("faults", "a corrupt completion is counted and its job recovered", [&](Metrics&) {
    spit(g_attr + "/hang_timeout_ms", "500");
    auto buf = dev.alloc(4096);
    long bad0 = stats()["bad_cqes"];
    inject(LACC_FAULT_BAD_CQE);
    auto r = dev.wait(dev.fill(buf, 1, 4096), 5000);
    auto r2 = dev.wait(dev.fill(buf, 2, 4096));
    spit(g_attr + "/hang_timeout_ms", "2000");
    return R{stats()["bad_cqes"] == bad0 + 1 && r.result == LACC_WAIT_ABORTED && r2.ok(),
                     "bad completion counted; job aborted by the watchdog; next job ok"};
  });
  check("faults", "a thermal event throttles and is reported", [&](Metrics&) {
    long t0 = stats()["thermal_events"];
    inject(LACC_FAULT_THERMAL);
    usleep(100000);
    return R{stats()["thermal_events"] == t0 + 1 && attr_l("throttled") == 1, "thermal interrupt and throttle"};
  });
}

void concurrency_checks() {
  check("concurrency", "8 threads, each with its own open file, submitting at once", [&](Metrics& m) {
    std::atomic<int> errors{0}, jobs{0};
    std::vector<std::thread> ts;
    auto t0 = Clock::now();
    for (int t = 0; t < 8; ++t)
      ts.emplace_back([&, t] {
        try {
          lacc::Device own("/dev/lacc0");
          auto buf = own.alloc(1 << 20);
          for (int i = 0; i < 60; ++i) {
            auto src = pattern(65536, t * 1000 + i);
            std::vector<uint8_t> dst(src.size());
            own.sync(own.write(buf, src.data(), src.size()));
            own.sync(own.read(buf, dst.data(), dst.size()));
            if (src != dst) ++errors;
            ++jobs;
          }
        } catch (...) {
          ++errors;
        }
      });
    for (auto& t : ts) t.join();
    m["jobs"] = jobs;
    m["seconds"] = ms_since(t0) / 1000;
    return R{errors == 0, std::to_string(jobs.load()) + " round trips from 8 threads, " + std::to_string(errors.load()) + " errors"};
  });
}

struct Perf {
  long cmds = 0, dma_kb = 0, mmacs = 0, busy_us = 0, irqs = 0;
};

// The card's own counters: work done and modelled busy time.
Perf perf() {
  Perf p;
  std::istringstream is(slurp(g_attr + "/perf"));
  std::string k;
  long v;
  while (is >> k >> v) {
    if (k == "cmds") p.cmds = v;
    if (k == "dma_kb") p.dma_kb = v;
    if (k == "mmacs") p.mmacs = v;
    if (k == "busy_us") p.busy_us = v;
    if (k == "irqs") p.irqs = v;
  }
  return p;
}

void perf_checks(lacc::Device& dev) {
  check("perf", "host-to-card bandwidth: one SG descriptor vs one descriptor per segment", [&](Metrics& m) {
    const size_t sz = 16 << 20;
    auto buf = dev.alloc(sz);
    std::vector<uint8_t> src(sz, 1);
    struct Run {
      double model_gbps, wall_gbps, descriptors;
    };
    auto run = [&](lacc::Mode mode) {
      auto p0 = perf();
      long d0 = stats()["descriptors"];
      auto t0 = Clock::now();
      for (int i = 0; i < 5; ++i) dev.sync(dev.write(buf, src.data(), sz, 0, mode));
      double wall_s = ms_since(t0) / 1e3;
      auto p1 = perf();
      double bytes = double(p1.dma_kb - p0.dma_kb) * 1024;
      return Run{bytes / ((p1.busy_us - p0.busy_us) * 1e-6) / 1e9, 5.0 * sz / wall_s / 1e9,
                 double(stats()["descriptors"] - d0) / 5};
    };
    auto sg = run(lacc::Mode::ScatterGather), ps = run(lacc::Mode::PerSegment);
    m["sg_model_gbps"] = sg.model_gbps;
    m["per_segment_model_gbps"] = ps.model_gbps;
    m["per_segment_descriptors"] = ps.descriptors;
    m["sg_emulated_wall_gbps"] = sg.wall_gbps;
    char b[260];
    std::snprintf(b, sizeof(b),
                  "modelled: SG %.2f GB/s, per-segment %.2f GB/s (%.0f descriptors per 16 MiB); emulated wall time: %.2f GB/s",
                  sg.model_gbps, ps.model_gbps, ps.descriptors, sg.wall_gbps);
    return R{sg.model_gbps >= ps.model_gbps && sg.model_gbps > 10, std::string(b)};
  });
  check("perf", "interrupt coalescing: interrupts under load against latency of a lone job", [&](Metrics& m) {
    auto buf = dev.alloc(4096);
    auto run = [&](const char* setting, const char* key) {
      spit(g_attr + "/coalesce", setting);
      // a burst of 512 small jobs: how many interrupts does the host take?
      long i0 = stats()["irqs"];
      std::vector<uint64_t> f;
      for (int i = 0; i < 512; ++i) f.push_back(dev.fill(buf, i, 4096));
      for (auto x : f) dev.wait(x);
      long n = stats()["irqs"] - i0;
      // one job at a time: how long until the waiter wakes?
      std::vector<double> lat;
      for (int i = 0; i < 64; ++i) {
        auto t0 = Clock::now();
        dev.wait(dev.fill(buf, i, 4096));
        lat.push_back(ms_since(t0) * 1e3);
      }
      std::sort(lat.begin(), lat.end());
      m[std::string("irqs_") + key] = n;
      m[std::string("lone_p50_us_") + key] = lat[lat.size() / 2];
      return std::pair{n, lat[lat.size() / 2]};
    };
    auto [n0, l0] = run("0 0", "off");
    auto [n1, l1] = run("8 200", "8_200us");
    auto [n2, l2] = run("32 1000", "32_1ms");
    spit(g_attr + "/coalesce", "0 0");
    char b[300];
    std::snprintf(b, sizeof(b),
                  "interrupts per 512-job burst: %ld off, %ld at 8 jobs/200 us, %ld at 32 jobs/1 ms; "
                  "lone-job p50 wake-up: %.0f, %.0f, %.0f us",
                  n0, n1, n2, l0, l1, l2);
    // coalescing must cut interrupts under load, and a lone job pays the timer
    return R{n2 < n0 && l2 > l0, std::string(b)};
  });
  check("perf", "GEMM throughput (modelled device time)", [&](Metrics& m) {
    const uint32_t S = 512;
    auto A = dev.alloc(S * S), B = dev.alloc(S * S), C = dev.alloc(S * S * 4);
    auto tops = [&](lacc::Kind kind) {
      auto p0 = perf();
      for (int i = 0; i < 4; ++i) dev.sync(dev.gemm(A, B, C, S, S, S, kind));
      auto p1 = perf();
      return 2.0 * (p1.mmacs - p0.mmacs) * 1e6 / ((p1.busy_us - p0.busy_us) * 1e-6) / 1e12;
    };
    double lns = tops(lacc::Kind::Lns8), i8 = tops(lacc::Kind::Int8);
    m["lns8_tops"] = lns;
    m["int8_tops"] = i8;
    char b[200];
    std::snprintf(b, sizeof(b), "512^3: LNS8 %.1f TOPS, INT8 %.1f TOPS at %s MHz (model: 16384 MACs per clock)", lns, i8,
                  slurp(g_attr + "/clock_mhz").c_str());
    return R{lns > 1 && i8 > 1, std::string(b)};
  });
}

}  // namespace

int main(int argc, char** argv) {
  std::string only = argc > 1 ? argv[1] : "";
  pci_checks();
  if (g_sys.empty()) return 1;
  try {
    lacc::Device dev("/dev/lacc0");
    auto want = [&](const char* g) { return only.empty() || only == g; };
    if (want("driver")) driver_checks(dev);
    if (want("dma")) dma_checks(dev);
    if (want("compute")) gemm_checks(dev);
    if (want("board")) board_checks();
    if (want("concurrency")) concurrency_checks();
    if (want("perf")) perf_checks(dev);
    if (want("power")) power_checks(dev);
    if (want("faults")) fault_checks(dev);
  } catch (const std::exception& e) {
    emit("driver", "open /dev/lacc0", false, e.what());
  }
  return g_failed ? 1 : 0;
}
