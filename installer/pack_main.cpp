// Build tool: packs directories into the installer payload.
//   ft_pack_payload <out.bin> <prefix>=<dir> [<prefix>=<dir> ...]
// Every file under <dir> is stored as "<prefix>/<path relative to dir>".
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/path_utf8.h"
#include "payload.h"

namespace fs = std::filesystem;

int wmain(int argc, wchar_t** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: ft_pack_payload <out.bin> <prefix>=<dir> ...\n");
    return 2;
  }
  std::vector<ft::installer::PayloadFile> files;
  for (int i = 2; i < argc; ++i) {
    const std::wstring arg = argv[i];
    const std::size_t eq = arg.find(L'=');
    if (eq == std::wstring::npos) {
      std::fprintf(stderr, "bad argument: expected <prefix>=<dir>\n");
      return 2;
    }
    const std::string prefix = ft::pathToUtf8(fs::path(arg.substr(0, eq)));
    const fs::path dir = arg.substr(eq + 1);
    std::vector<fs::path> paths;
    for (const auto& e : fs::recursive_directory_iterator(dir))
      if (e.is_regular_file()) paths.push_back(e.path());
    std::sort(paths.begin(), paths.end());  // reproducible payloads
    for (const fs::path& p : paths) {
      ft::installer::PayloadFile f;
      f.path = prefix + "/" + ft::pathToUtf8(p.lexically_relative(dir).generic_wstring());
      std::ifstream in(p, std::ios::binary);
      f.data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
      files.push_back(std::move(f));
    }
  }
  std::string error;
  if (!ft::installer::writePayload(argv[1], files, &error)) {
    std::fprintf(stderr, "ft_pack_payload: %s\n", error.c_str());
    return 1;
  }
  std::printf("packed %zu files into %s\n", files.size(), ft::pathToUtf8(argv[1]).c_str());
  return 0;
}
