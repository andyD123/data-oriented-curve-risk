#pragma once
// Examples read and write data files by relative path. CMake defines LADDER_EXAMPLE_DIR as the example's build
// directory (where its data files are copied); start there so a run works from any working directory, including an
// IDE run configuration. Built without CMake, this does nothing and the example runs in the current directory.
#include <filesystem>
#include <cstdio>
inline void enter_example_dir() {
#if defined(LADDER_EXAMPLE_DIR)
    std::error_code ec; std::filesystem::current_path(LADDER_EXAMPLE_DIR, ec);
    if (ec) std::fprintf(stderr, "cannot enter %s: %s\n", LADDER_EXAMPLE_DIR, ec.message().c_str());
#endif
}
