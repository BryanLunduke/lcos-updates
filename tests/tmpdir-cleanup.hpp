// Round 14 (U-6): every test removes the /tmp/lcos-* folder it made when it
// exits, so runs leave nothing behind (one held a fake password in term.log).
#pragma once
#include <cstdlib>
#include <cstring>
#include <ftw.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace lcos_test {

inline std::vector<std::string>& tmp_dirs()
{
  static std::vector<std::string> dirs;
  return dirs;
}

inline int remove_entry(const char* path, const struct stat*, int, struct FTW*)
{
  ::remove(path);
  return 0;
}

inline pid_t& owner_pid()
{
  static pid_t pid = 0;
  return pid;
}

inline void remove_tmp_dirs()
{
  /* A forked child that calls exit() must not remove its parent's folder. */
  if (getpid() != owner_pid())
    return;
  for (const auto& dir : tmp_dirs())
    /* Only our own mkdtemp folders; never cross into another filesystem. */
    if (dir.compare(0, 10, "/tmp/lcos-") == 0 && dir.find("/..") == std::string::npos)
      nftw(dir.c_str(), remove_entry, 16, FTW_DEPTH | FTW_PHYS | FTW_MOUNT);
  tmp_dirs().clear();
}

/* Call right after mkdtemp succeeds. Set LCOS_TEST_KEEP_TMP=1 to keep it. */
inline void cleanup_at_exit(const char* dir)
{
  const char* keep = std::getenv("LCOS_TEST_KEEP_TMP");
  if (keep != nullptr && std::strcmp(keep, "1") == 0)
    return;
  if (tmp_dirs().empty()) {
    owner_pid() = getpid();
    std::atexit(remove_tmp_dirs);
  }
  tmp_dirs().push_back(dir);
}

} // namespace lcos_test
