#pragma once
#include "record.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" int LZ4_decompress_safe(const char*, char*, int, int);

namespace nanhang_weighted_fetch {
namespace {

using Byte = std::uint8_t;
using Clock = std::chrono::steady_clock;
using U64 = std::uint64_t;

static_assert(std::endian::native == std::endian::little,
              "the persisted prepared arrays are little-endian");
static_assert(offsetof(Returned54, values) == kSamples * sizeof(std::int64_t));
static_assert(sizeof(Returned54) ==
              kSamples * sizeof(std::int64_t) + kDimensions * sizeof(float));

[[noreturn]] void fail(std::string_view message) {
  throw std::runtime_error(std::string("weighted fetch: ") + std::string(message));
}

inline void require(bool value, std::string_view message) {
  if (!value) fail(message);
}

constexpr int decode_zigzag(unsigned value) {
  return static_cast<int>(value >> 1) ^ -static_cast<int>(value & 1);
}
static_assert(decode_zigzag(0) == 0);
static_assert(decode_zigzag(1) == -1);
static_assert(decode_zigzag(2) == 1);
static_assert(decode_zigzag(3) == -2);

std::uint64_t ns(Clock::time_point begin, Clock::time_point end) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

class Mapping {
 public:
  explicit Mapping(const fs::path& path) : path_(path), size_(fs::file_size(path)) {
    require(size_ > 0, "empty file " + path.string());
    const int fd = ::open(path.c_str(), O_RDONLY);
    require(fd >= 0, "open " + path.string());
    void* address = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    require(address != MAP_FAILED, "mmap " + path.string());
    data_ = static_cast<const Byte*>(address);
  }

  ~Mapping() {
    if (data_) ::munmap(const_cast<Byte*>(data_), size_);
  }

  Mapping(const Mapping&) = delete;
  Mapping& operator=(const Mapping&) = delete;
  Mapping(Mapping&& other) noexcept
      : path_(std::move(other.path_)), data_(other.data_), size_(other.size_) {
    other.data_ = nullptr;
    other.size_ = 0;
  }

  const Byte* data() const { return data_; }
  std::size_t size() const { return size_; }

 private:
  fs::path path_;
  const Byte* data_ = nullptr;
  std::size_t size_ = 0;
};

struct Span {
  const Byte* p = nullptr;
  const Byte* end = nullptr;

  std::size_t left() const { return static_cast<std::size_t>(end - p); }
  const Byte* take(std::size_t n) {
    require(n <= left(), "truncated file");
    const Byte* result = p;
    p += n;
    return result;
  }
  unsigned byte() { return *take(1); }
  unsigned var() {
    unsigned value = 0;
    for (unsigned shift = 0; shift <= 28; shift += 7) {
      const unsigned part = byte();
      value |= (part & 127u) << shift;
      if (part < 128) return value;
    }
    fail("invalid varint");
  }
  std::string str() {
    const unsigned n = var();
    require((n & 1u) == 0, "null path segment");
    const auto* begin = reinterpret_cast<const char*>(take(n / 2));
    return std::string(begin, n / 2);
  }
};

unsigned be_u16(const Byte* source) {
  return (unsigned(source[0]) << 8) | source[1];
}

unsigned be_u32(const Byte* source) {
  return std::uint32_t(source[0]) << 24 |
         std::uint32_t(source[1]) << 16 |
         std::uint32_t(source[2]) << 8 | source[3];
}

U64 be_word(const Byte* source, unsigned bytes) {
  return bytes == 4 ? be_u32(source)
                    : (U64(be_u32(source)) << 32) | be_u32(source + 4);
}

struct Page {
  const Byte* compressed = nullptr;
  unsigned size = 0;
  unsigned plain = 0;
  unsigned codec = 0;

  Span unpack(std::vector<Byte>& scratch) const {
    require(compressed != nullptr, "missing page");
    if (codec == 0) {
      require(size == plain, "invalid uncompressed page extent");
      return {compressed, compressed + size};
    }
    require(codec == 7, "only IoTDB LZ4 page compression is supported");
    scratch.resize(plain);
    const int decoded = LZ4_decompress_safe(
        reinterpret_cast<const char*>(compressed),
        reinterpret_cast<char*>(scratch.data()), static_cast<int>(size),
        static_cast<int>(plain));
    require(decoded == static_cast<int>(plain), "LZ4 page decode failed");
    return {scratch.data(), scratch.data() + plain};
  }
};

struct ScalingLane {
  Span pack;
  double scale = 1.0;
};

struct RankPageInfo {
  unsigned width = 0;
  const float* table = nullptr;
  std::size_t table_size = 0;
  std::array<unsigned char, kSamples> k{};
  std::array<const Byte*, 9> edge{};
};

struct AnchorRankPageInfo {
  unsigned width = 0;
  unsigned coordinate_width = 0;
  unsigned rows = 0;
  const float* table = nullptr;
  std::size_t table_size = 0;
  std::array<unsigned char, kSamples> k{};
  const Byte* anchor = nullptr;
  const Byte* offsets = nullptr;
  const Byte* coordinate_starts = nullptr;
  const Byte* payload = nullptr;
  const Byte* end = nullptr;
};

#ifdef TS_RESTART
struct TimeRestart {
  std::uint32_t offset;
  std::uint16_t start;
  std::uint16_t count;
  std::array<U64,4> values;
};
#endif
struct Block {
#ifdef TS_RESTART
  std::vector<TimeRestart> time_packs;
#endif
  bool plain_time = false;
  std::array<const Byte*,3> plain_signals{};
  std::array<unsigned,3> signal_encoding{};
  std::string device;
  Page time;
  Page ids;
  std::array<Page, 3> signals;
  unsigned count = 0;
  std::unique_ptr<std::array<ScalingLane, kDimensions>> lanes;
  std::unique_ptr<std::array<RankPageInfo, 3>> ranks;
  std::unique_ptr<std::array<AnchorRankPageInfo, 3>> anchor_ranks;
};

void index_file(const Mapping& file, std::map<int, Block>& blocks) {
  require(file.size() > 13 &&
              std::memcmp(file.data(), "TsFile\4", 7) == 0 &&
              std::memcmp(file.data() + file.size() - 6, "TsFile", 6) == 0,
          "not a sealed TsFile v4");
  Span in{file.data() + 7, file.data() + file.size()};
  Block* block = nullptr;
  for (;;) {
    const unsigned tag = in.byte();
    if (tag == 2) break;
    if (tag == 4) {
      in.take(16);
      continue;
    }
    if (tag == 0) {
      const unsigned n = in.var();
      std::string leaf;
      std::string device;
      for (unsigned j = 0; j < n; ++j) {
        leaf = in.str();
        if (j) device += '.';
        device += leaf;
      }
      require(leaf.size() > 1 && leaf[0] == 'b',
              "unexpected device " + leaf);
      block = &blocks[std::stoi(leaf.substr(1))];
      block->device = std::move(device);
      continue;
    }
    require(block && (tag == 0x85 || tag == 0x45),
            "expected one sealed page per chunk");
    const std::string name = in.str();
    const unsigned bytes = in.var();
    const unsigned type = in.byte();
    const unsigned codec = in.byte();
    const unsigned encoding = in.byte();
    if (tag == 0x85) {
      block->plain_time = encoding == 0;
      require(encoding == 4 || (block->plain_time && codec == 0), "unsupported time page");
    } else {
      require(encoding == 4 || (name != "vector_id" && encoding == 0 && codec == 0),
              "signal or ID encoding changed");
    }
    const auto* chunk = in.take(bytes);
    Span page{chunk, chunk + bytes};
    const unsigned plain = page.var();
    const unsigned stored = page.var();
    Page* destination = nullptr;
    if (tag == 0x85) {
      require(type == 6, "time column is not INT64");
      destination = &block->time;
    } else if (name == "vector_id") {
      require(type == 1, "vector_id is not INT32");
      destination = &block->ids;
    } else {
      const unsigned signal = name == "mp" ? 0 : name == "ip" ? 1 :
                              name == "n2" ? 2 : 3;
      require(signal < 3 && type == 3, "unexpected signal column");
      block->signal_encoding[signal] = encoding;
      destination = &block->signals[signal];
    }
    require(destination->compressed == nullptr && stored == page.left(),
            "split or duplicate page");
    *destination = {page.take(stored), stored, plain, codec};
  }
}

template <class Emit>
void decode_ts2diff(Span in, unsigned bytes, unsigned start, unsigned n,
                    Emit emit) {
  unsigned done = 0;
  while (done < n) {
    const unsigned deltas = be_u32(in.take(4));
    const unsigned width = be_u32(in.take(4));
    require(deltas <= 128 && width <= bytes * 8, "invalid TS_2DIFF pack");
    const U64 minimum = be_word(in.take(bytes), bytes);
    U64 value = be_word(in.take(bytes), bytes);
    const auto* bits = in.take((std::size_t(deltas) * width + 7) / 8);
    const unsigned count = deltas + 1;
    if (start >= count) {
      start -= count;
      continue;
    }
    const unsigned limit = std::min(count, start + n - done);
    unsigned bit = 0;
    for (unsigned row = 0; row < limit; ++row) {
      if (row) {
        U64 residual = 0;
        unsigned remaining = width;
        while (remaining) {
          const unsigned take = std::min(remaining, 8u - bit % 8);
          const unsigned shift = 8 - bit % 8 - take;
          residual = (residual << take) |
                     ((bits[bit / 8] >> shift) & ((1u << take) - 1));
          remaining -= take;
          bit += take;
        }
        value += minimum + residual;
      }
      if (row >= start) emit(done++, value);
    }
    start = 0;
  }
}

unsigned bitmap(Span& in, const Byte*& data) {
  const unsigned rows = be_u32(in.take(4));
  data = in.take((rows + 7) / 8);
  return rows;
}

std::vector<unsigned> ids_from(const Page& page, std::vector<Byte>& scratch) {
  auto in = page.unpack(scratch);
  const Byte* validity = nullptr;
  const unsigned rows = bitmap(in, validity);
  require(rows && rows <= 128 * kSamples && rows % kSamples == 0,
          "invalid aligned vector_id page length");
  for (unsigned i = 0; i < rows; ++i) {
    const bool present = (validity[i / 8] & (128u >> (i % 8))) != 0;
    require(present == (i % kSamples == 0), "vector_id alignment mismatch");
  }
  std::vector<unsigned> ids(rows / kSamples);
  decode_ts2diff(in, 4, 0, ids.size(),
                 [&](unsigned i, U64 value) { ids[i] = std::uint32_t(value); });
  return ids;
}

#include "ts_time_reader.inc"

void read_times(const Block& block, unsigned time_slot,
                std::int64_t* out, std::vector<Byte>& scratch) {
  if (block.plain_time) {
    const auto page = block.time.unpack(scratch);
    const std::size_t offset = time_slot * kSamples * sizeof(std::int64_t);
    require(offset + kSamples * sizeof(std::int64_t) <= page.left(), "PLAIN timestamp bounds");
    for (unsigned j=0; j<kSamples; ++j) {
      U64 word;
      std::memcpy(&word, page.p+offset+8*j, 8);
      out[j] = std::bit_cast<std::int64_t>(__builtin_bswap64(word));
    }
    return;
  }
  if (block.time.codec == 0) {
    decode_random_times(block,block.time.unpack(scratch),time_slot*kSamples,kSamples,out);
    return;
  }
  decode_ts2diff(block.time.unpack(scratch), 8, time_slot * kSamples,
                 kSamples, [&](unsigned i, U64 value) {
                   out[i] = static_cast<std::int64_t>(value);
                 });
}

std::vector<fs::path> files_with_suffix(const fs::path& directory,
                                        std::string_view suffix) {
  require(fs::is_directory(directory), "missing directory " + directory.string());
  std::vector<fs::path> result;
  for (const auto& entry : fs::directory_iterator(directory)) {
    if (entry.is_regular_file() || entry.is_symlink()) {
      const std::string value = entry.path().string();
      if (value.size() >= suffix.size() &&
          value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0)
        result.push_back(entry.path());
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

std::uint64_t total_size(const std::vector<fs::path>& paths) {
  std::uint64_t result = 0;
  for (const auto& path : paths) result += fs::file_size(path);
  return result;
}

struct AddressFile {
  Mapping mapping;
  const std::uint32_t* locations = nullptr;
  std::uint32_t rows = 0;
  std::uint32_t blocks = 0;

  explicit AddressFile(const fs::path& path) : mapping(path) {
    require(mapping.size() >= 24 &&
                std::memcmp(mapping.data(), "NHADDR1\0", 8) == 0,
            "invalid sim address magic");
    std::uint32_t version = 0;
    std::memcpy(&version, mapping.data() + 8, 4);
    std::memcpy(&rows, mapping.data() + 12, 4);
    std::memcpy(&blocks, mapping.data() + 16, 4);
    require(version == 1 && rows > 0 && blocks > 0,
            "unsupported sim address header");
    require(mapping.size() == 24 + std::size_t(rows) * 4,
            "sim address extent mismatch");
    locations = reinterpret_cast<const std::uint32_t*>(mapping.data() + 24);
  }
};

}  // namespace

} // namespace nanhang_weighted_fetch
