#include "core/path_utf8.h"

#include <exception>

namespace ft {

std::string pathToUtf8(const std::filesystem::path& path) {
  // C++20 u8string() returns std::u8string, so copy the bytes. Called from per-frame UI
  // code, so it must not throw (u8string() can, for ill-formed UTF-16 such as an unpaired
  // surrogate in a Windows file name).
  try {
    const std::u8string u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
  } catch (const std::exception&) {
    return "(path not representable as UTF-8)";
  }
}

std::filesystem::path pathFromUtf8(std::string_view utf8) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

}  // namespace ft
