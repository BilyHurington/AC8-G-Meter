#pragma once
// PNG writing through the Windows Imaging Component (needs COM initialised on the thread).
#include <cstdint>
#include <filesystem>
#include <string>

namespace ft {

// `bgra`: width * height pixels, rows tightly packed. False with *error on failure.
bool writePngBgra(const std::filesystem::path& path, unsigned width, unsigned height, const std::uint8_t* bgra,
                  std::string* error = nullptr);

}  // namespace ft
