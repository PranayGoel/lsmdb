// The single most important test in this project: proves durability across
// a REAL OS-level process kill (SIGKILL on POSIX, TerminateProcess on
// Windows), not a simulated crash within this test binary. No destructors
// run in the killed process, no cleanup happens -- exactly what an actual
// crash looks like.
//
// The process-spawn/pipe/kill code below is deliberately NOT behind the
// project's platform::sync_file abstraction (see include/lsmdb/platform/
// file_sync.h's module comment, which says that's the ONE place OS-specific
// code belongs in the engine itself) -- this file is test infrastructure,
// not engine code, and spawning/killing a subprocess to test the engine is a
// different concern from the engine's own portability story. Kept isolated
// to this one test file with clear #ifdef branches rather than pretending a
// shared abstraction is warranted for a single, one-off use.
#include "lsmdb/db.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <random>
#include <string>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdio>
#endif

using lsmdb::Db;

namespace {

std::filesystem::path make_temp_dir() {
  std::random_device rd;
  auto dir = std::filesystem::temp_directory_path() / ("lsmdb_crash_test_" + std::to_string(rd()));
  std::filesystem::create_directories(dir);
  return dir;
}

#if defined(_WIN32)

// Spawns crash_helper, blocks until it prints "all-writes-complete" on
// stdout (proving every put() has durably returned), then force-kills it.
void spawn_write_and_kill(const std::string& helper_path, const std::string& dir, int num_keys) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE read_pipe, write_pipe;
  REQUIRE(CreatePipe(&read_pipe, &write_pipe, &sa, 0));
  SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags |= STARTF_USESTDHANDLES;
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe;

  PROCESS_INFORMATION pi{};
  std::string cmdline = "\"" + helper_path + "\" \"" + dir + "\" " + std::to_string(num_keys);
  std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
  cmdline_buf.push_back('\0');

  BOOL ok = CreateProcessA(nullptr, cmdline_buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                            &si, &pi);
  CloseHandle(write_pipe);
  REQUIRE(ok);

  // Block reading the pipe until the child's completion line arrives.
  std::string output;
  char buf[256];
  DWORD bytes_read;
  while (output.find("all-writes-complete") == std::string::npos) {
    if (!ReadFile(read_pipe, buf, sizeof(buf), &bytes_read, nullptr) || bytes_read == 0) break;
    output.append(buf, bytes_read);
  }
  CloseHandle(read_pipe);

  TerminateProcess(pi.hProcess, 1);
  WaitForSingleObject(pi.hProcess, INFINITE);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
}

#else

void spawn_write_and_kill(const std::string& helper_path, const std::string& dir, int num_keys) {
  int fds[2];
  REQUIRE(pipe(fds) == 0);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    // Child: redirect stdout to the pipe's write end, then exec the helper.
    dup2(fds[1], STDOUT_FILENO);
    close(fds[0]);
    close(fds[1]);
    execl(helper_path.c_str(), helper_path.c_str(), dir.c_str(), std::to_string(num_keys).c_str(),
          static_cast<char*>(nullptr));
    _exit(127);  // only reached if execl itself failed
  }

  // Parent: block reading the pipe until the completion line arrives.
  close(fds[1]);
  std::string output;
  char buf[256];
  while (output.find("all-writes-complete") == std::string::npos) {
    ssize_t n = read(fds[0], buf, sizeof(buf));
    if (n <= 0) break;
    output.append(buf, static_cast<size_t>(n));
  }
  close(fds[0]);

  kill(pid, SIGKILL);
  int status;
  waitpid(pid, &status, 0);
}

#endif

}  // namespace

TEST_CASE(
    "every write acknowledged before a hard SIGKILL/TerminateProcess survives, "
    "recovered by a freshly opened Db in a completely separate process",
    "[db][crash]") {
  auto dir = make_temp_dir();
  constexpr int kNumKeys = 25;

  spawn_write_and_kill(CRASH_HELPER_PATH, dir.string(), kNumKeys);

  {
    // Scoped so `recovered`'s open WAL/SSTable handles close before
    // remove_all runs below -- Windows can't delete a file with an open
    // handle on it, unlike POSIX.
    //
    // A brand-new Db, in THIS process, opened on the directory the killed
    // process was writing to -- this is the actual recovery path being
    // proven, identical to what would happen restarting a real database
    // server after its process was killed.
    Db recovered(dir);
    for (int i = 0; i < kNumKeys; ++i) {
      auto result = recovered.get("crash-key-" + std::to_string(i));
      REQUIRE(result.has_value());
      REQUIRE(*result == "crash-value-" + std::to_string(i));
    }
  }

  std::filesystem::remove_all(dir);
}
