#pragma once
// The installer's embedded payload: a list of files, each compressed with the Windows
// Compression API (LZMS, no third-party code). Written at build time by ft_pack_payload,
// read by the installer from its own resources.
//
// Layout (little endian): "AC8GMPK1" | u32 count | per file:
//   u32 pathBytes | path (UTF-8, '/' separated) | u64 size | u64 packedSize | packed bytes
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace ft::installer {

struct PayloadFile {
  std::string path;  // e.g. "ue4ss/UE4SS/UE4SS.dll"
  std::vector<std::uint8_t> data;
};

bool writePayload(const std::filesystem::path& out, const std::vector<PayloadFile>& files, std::string* error);
bool readPayload(std::span<const std::uint8_t> bytes, std::vector<PayloadFile>& out, std::string* error);

}  // namespace ft::installer
