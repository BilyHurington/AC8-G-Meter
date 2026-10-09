#include "payload.h"

#include <windows.h>

#include <compressapi.h>

#include <cstring>
#include <fstream>

namespace ft::installer {

namespace {

constexpr char kMagic[8] = {'A', 'C', '8', 'G', 'M', 'P', 'K', '1'};
constexpr std::uint32_t kMaxPathBytes = 1024;
constexpr std::uint64_t kMaxFileBytes = 256ull << 20;  // far above the largest file (UE4SS.dll, 20 MB)

// Relative, '/' separated, no "..", "." or empty parts, no drive or root.
bool isSafePath(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos ||
      path.find(':') != std::string_view::npos)
    return false;
  for (std::size_t start = 0;;) {
    const std::size_t end = path.find('/', start);
    const std::string_view part = path.substr(start, end - start);
    if (part.empty() || part == "." || part == "..") return false;
    if (end == std::string_view::npos) return true;
    start = end + 1;
  }
}

template <class T>
void put(std::vector<std::uint8_t>& out, T v) {
  const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
  out.insert(out.end(), p, p + sizeof v);
}

class Reader {
 public:
  explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
  bool take(void* dst, std::size_t n) {
    if (bytes_.size() - pos_ < n) return false;
    std::memcpy(dst, bytes_.data() + pos_, n);
    pos_ += n;
    return true;
  }
  template <class T>
  bool get(T& v) {
    return take(&v, sizeof v);
  }
  std::span<const std::uint8_t> span(std::size_t n) {
    if (bytes_.size() - pos_ < n) return {};
    auto s = bytes_.subspan(pos_, n);
    pos_ += n;
    return s;
  }
  std::size_t remaining() const { return bytes_.size() - pos_; }

 private:
  std::span<const std::uint8_t> bytes_;
  std::size_t pos_ = 0;
};

bool fail(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}

// RAII handles of the Compression API.
struct Compressor {
  COMPRESSOR_HANDLE h = nullptr;
  ~Compressor() {
    if (h) CloseCompressor(h);
  }
};
struct Decompressor {
  DECOMPRESSOR_HANDLE h = nullptr;
  ~Decompressor() {
    if (h) CloseDecompressor(h);
  }
};

}  // namespace

bool writePayload(const std::filesystem::path& out, const std::vector<PayloadFile>& files, std::string* error) {
  Compressor c;
  if (!CreateCompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &c.h)) return fail(error, "CreateCompressor failed");
  std::vector<std::uint8_t> blob(kMagic, kMagic + sizeof kMagic);
  put(blob, static_cast<std::uint32_t>(files.size()));
  std::vector<std::uint8_t> packed;
  for (const PayloadFile& f : files) {
    if (f.data.empty()) {  // the Compression API rejects empty input: stored as 0 / 0
      put(blob, static_cast<std::uint32_t>(f.path.size()));
      blob.insert(blob.end(), f.path.begin(), f.path.end());
      put(blob, std::uint64_t{0});
      put(blob, std::uint64_t{0});
      continue;
    }
    SIZE_T needed = 0;
    // Asking for the size with no output buffer fails with ERROR_INSUFFICIENT_BUFFER.
    Compress(c.h, f.data.data(), f.data.size(), nullptr, 0, &needed);
    packed.resize(needed);
    SIZE_T packedSize = 0;
    if (!Compress(c.h, f.data.data(), f.data.size(), packed.data(), packed.size(), &packedSize))
      return fail(error, "compressing " + f.path + " failed");
    put(blob, static_cast<std::uint32_t>(f.path.size()));
    blob.insert(blob.end(), f.path.begin(), f.path.end());
    put(blob, static_cast<std::uint64_t>(f.data.size()));
    put(blob, static_cast<std::uint64_t>(packedSize));
    blob.insert(blob.end(), packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(packedSize));
  }
  std::ofstream file(out, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(blob.data()), static_cast<std::streamsize>(blob.size()));
  if (!file) return fail(error, "writing the payload failed");
  return true;
}

bool readPayload(std::span<const std::uint8_t> bytes, std::vector<PayloadFile>& out, std::string* error) {
  out.clear();
  Reader r(bytes);
  char magic[sizeof kMagic];
  std::uint32_t count = 0;
  if (!r.take(magic, sizeof magic) || std::memcmp(magic, kMagic, sizeof magic) != 0 || !r.get(count))
    return fail(error, "not an AC8 G-Meter payload");
  Decompressor d;
  if (!CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &d.h)) return fail(error, "CreateDecompressor failed");
  for (std::uint32_t i = 0; i < count; ++i) {
    std::uint32_t pathBytes = 0;
    std::uint64_t size = 0, packedSize = 0;
    PayloadFile f;
    // Every size is checked before anything is allocated: a damaged resource must fail cleanly.
    if (!r.get(pathBytes) || pathBytes == 0 || pathBytes > kMaxPathBytes || pathBytes > r.remaining())
      return fail(error, "damaged payload");
    f.path.resize(pathBytes);
    if (!r.take(f.path.data(), pathBytes) || !r.get(size) || !r.get(packedSize) || size > kMaxFileBytes ||
        packedSize > r.remaining() || (size == 0) != (packedSize == 0))
      return fail(error, "damaged payload");
    if (!isSafePath(f.path)) return fail(error, "payload path not allowed: " + f.path);
    const std::span<const std::uint8_t> packed = r.span(static_cast<std::size_t>(packedSize));
    f.data.resize(static_cast<std::size_t>(size));
    if (size == 0) {
      out.push_back(std::move(f));
      continue;
    }
    SIZE_T written = 0;
    if (!Decompress(d.h, packed.data(), packed.size(), f.data.data(), f.data.size(), &written) || written != size)
      return fail(error, "payload file " + f.path + " is corrupt");
    out.push_back(std::move(f));
  }
  return true;
}

}  // namespace ft::installer
