// The firmware image format: the C layout the boot ROM reads equals what fw/mkfw.py writes.
#include <gtest/gtest.h>
#include <zlib.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

extern "C" {
#include "lacc_hw.h"
}

namespace {

std::string tmpdir() {
  const char* t = std::getenv("TMPDIR");
  return t ? t : "/tmp";
}

std::vector<uint8_t> mkfw(const std::string& args) {
  const std::string out = tmpdir() + "/lacc_test_fw_" + std::to_string(::getpid()) + ".bin";
  const std::string cmd = std::string(LACC_PYTHON) + " " + LACC_MKFW + " " + args + " -o " + out;
  EXPECT_EQ(std::system(cmd.c_str()), 0) << cmd;
  std::ifstream f(out, std::ios::binary);
  std::vector<uint8_t> v((std::istreambuf_iterator<char>(f)), {});
  std::remove(out.c_str());
  return v;
}

lacc_fw_header header(const std::vector<uint8_t>& img) {
  lacc_fw_header h{};
  std::memcpy(&h, img.data(), sizeof(h));
  return h;
}

uint32_t header_crc(lacc_fw_header h) {
  h.header_crc32 = 0;
  return static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(&h), sizeof(h)));
}

}  // namespace

TEST(Firmware, LayoutIsFixed) {
  static_assert(sizeof(lacc_fw_header) == 56);
  static_assert(sizeof(lacc_flash_config) == 16);
  static_assert(sizeof(lacc_desc) == 64);
  static_assert(sizeof(lacc_cqe) == 16);
  static_assert(offsetof(lacc_fw_header, sha256) == 16);
  static_assert(offsetof(lacc_fw_header, header_crc32) == 52);
  SUCCEED();
}

TEST(Firmware, ImageHeaderParsesAndChecks) {
  auto img = mkfw("image --version 7 --rollback 3");
  ASSERT_GE(img.size(), sizeof(lacc_fw_header));
  auto h = header(img);
  EXPECT_EQ(h.magic, LACC_FW_MAGIC);
  EXPECT_EQ(h.version, 7u);
  EXPECT_EQ(h.rollback_index, 3u);
  EXPECT_EQ(h.payload_len, img.size() - sizeof(h));
  EXPECT_EQ(h.header_crc32, header_crc(h));
}

TEST(Firmware, TlvsCarryTheLutAndLimits) {
  auto img = mkfw("image --version 2 --rollback 1");
  auto h = header(img);
  size_t off = sizeof(h), end = off + h.payload_len;
  std::array<uint32_t, 8> lut{};
  uint32_t power[3]{}, seen = 0;
  while (off + 4 <= end) {
    lacc_tlv t{};
    std::memcpy(&t, &img[off], sizeof(t));
    off += sizeof(t);
    ASSERT_LE(off + t.len, end);
    if (t.type == LACC_TLV_LNS_LUT) std::memcpy(lut.data(), &img[off], 32);
    if (t.type == LACC_TLV_POWER) std::memcpy(power, &img[off], 12);
    seen |= 1u << t.type;
    off += (t.len + 3u) & ~3u;
  }
  EXPECT_EQ(seen, 0b111110u);
  EXPECT_EQ(lut[0], 65536u);
  EXPECT_EQ(lut[7], 120194u);
  EXPECT_EQ(power[0], 75000u);
}

TEST(Firmware, CorruptHeaderFailsTheCrc) {
  auto h = header(mkfw("image --version 9 --corrupt header"));
  EXPECT_NE(h.header_crc32, header_crc(h));
}

TEST(Firmware, FlashImageLayout) {
  auto a = mkfw("image --version 3 --rollback 2");
  const std::string ap = tmpdir() + "/lacc_test_a_" + std::to_string(::getpid()) + ".bin";
  std::ofstream(ap, std::ios::binary).write(reinterpret_cast<const char*>(a.data()), a.size());
  auto flash = mkfw("flash --a " + ap + " --boot-slot 1 --min-version 2");
  std::remove(ap.c_str());
  ASSERT_EQ(flash.size(), LACC_FLASH_SIZE);
  EXPECT_EQ(header(std::vector<uint8_t>(flash.begin(), flash.begin() + 64)).version, 3u);
  EXPECT_EQ(flash[LACC_FLASH_SLOT_B], 0xff);  // empty slot is erased flash
  lacc_flash_config c{};
  std::memcpy(&c, &flash[LACC_FLASH_CONFIG], sizeof(c));
  EXPECT_EQ(c.magic, LACC_CFG_MAGIC);
  EXPECT_EQ(c.boot_slot, 1u);
  EXPECT_EQ(c.min_version, 2u);
  EXPECT_EQ(c.crc32, static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(&c), 12)));
}
