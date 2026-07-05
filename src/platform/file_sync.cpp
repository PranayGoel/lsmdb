#include "lsmdb/platform/file_sync.h"

#include <chrono>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

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

bool remove_file(const std::filesystem::path& path) {
  // Ten attempts, 50ms apart: generous enough to ride out a transient
  // Defender scan (typically resolves within one or two retries in
  // practice) without turning a real failure into a multi-second hang.
  constexpr int kMaxAttempts = 10;
  constexpr auto kRetryDelay = std::chrono::milliseconds(50);

  std::error_code ec;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    bool removed = std::filesystem::remove(path, ec);
    if (!ec) return removed;
    std::this_thread::sleep_for(kRetryDelay);
  }
  throw std::runtime_error("remove_file: failed to remove " + path.string() + ": " + ec.message());
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

bool remove_file(const std::filesystem::path& path) {
  // POSIX unlink(2) has no "file in use" failure mode -- a file can be
  // removed while other handles remain open on it -- so no retry is needed.
  return std::filesystem::remove(path);
}

#endif

}  // namespace lsmdb::platform
