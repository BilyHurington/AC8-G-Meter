#pragma once
// Lossless conversion between std::filesystem::path and UTF-8 text.
// Never use path::string() or path(std::string) for that: on Windows they go through the
// ANSI code page, which loses (or throws for) characters outside it.
#include <filesystem>
#include <string>
#include <string_view>

namespace ft {

// UTF-8 text for showing a path in ImGui or an error message. Never throws.
std::string pathToUtf8(const std::filesystem::path& path);
// Inverse of pathToUtf8, e.g. for UTF-8 command-line arguments. Never throws for valid UTF-8.
std::filesystem::path pathFromUtf8(std::string_view utf8);

}  // namespace ft
