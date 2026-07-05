#include "lsmdb/platform/file_sync.h"

#include <stdexcept>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace lsmdb::platform {

#if defined(_WIN32)

void sync_file(const std::filesystem::path& path) {
  // FILE_FLAG_WRITE_THROUGH isn't required here since we call
  // FlushFileBuffers explicitly, but GENERIC_WRITE + OPEN_EXISTING is
  // required to get a HANDLE we're allowed to flush.
  HANDLE handle = CreateFileW(
      path.wstring().c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    throw std::runtime_error("sync_file: CreateFileW failed for " + path.string());
  }
  bool ok = FlushFileBuffers(handle);
  CloseHandle(handle);
  if (!ok) {
    throw std::runtime_error("sync_file: FlushFileBuffers failed for " + path.string());
  }
}

#else

void sync_file(const std::filesystem::path& path) {
  int fd = ::open(path.c_str(), O_WRONLY);
  if (fd < 0) {
    throw std::runtime_error("sync_file: open failed for " + path.string());
  }
  int result = ::fsync(fd);
  ::close(fd);
  if (result != 0) {
    throw std::runtime_error("sync_file: fsync failed for " + path.string());
  }
}

#endif

}  // namespace lsmdb::platform
