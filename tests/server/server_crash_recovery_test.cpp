// The Tier 2 equivalent of tests/core/crash_recovery_test.cpp: proves that
// data written through the actual network protocol survives a real,
// separately-spawned lsmdb_server process being hard-killed -- not a
// simulated crash, not an in-process test double, but the real server
// binary this project ships, killed the same way an orchestrator (or a
// crash) would kill it in production, then restarted fresh.
//
// Like crash_recovery_test.cpp, the process-spawn/pipe/kill code below is
// deliberately kept in this test file with plain #ifdef _WIN32 branches
// rather than folded into the engine's platform::sync_file-style
// abstraction -- that abstraction is scoped to what the database engine
// itself needs at runtime; spawning and killing a subprocess to prove a
// property is test infrastructure, a different concern.
#include <asio.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <istream>
#include <random>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

std::filesystem::path make_temp_dir() {
  std::random_device rd;
  auto dir = std::filesystem::temp_directory_path() /
             ("lsmdb_server_crash_test_" + std::to_string(rd()));
  std::filesystem::create_directories(dir);
  return dir;
}

std::string send_command(asio::ip::tcp::socket& socket, const std::string& command) {
  std::string line = command + "\r\n";
  asio::write(socket, asio::buffer(line));
  asio::streambuf buffer;
  asio::read_until(socket, buffer, "\r\n");
  std::istream is(&buffer);
  std::string response;
  std::getline(is, response);
  if (!response.empty() && response.back() == '\r') response.pop_back();
  return response;
}

#if defined(_WIN32)

struct SpawnedServer {
  PROCESS_INFORMATION pi{};
  HANDLE read_pipe = nullptr;
};

SpawnedServer spawn_server(const std::string& server_path, const std::string& dir) {
  SpawnedServer sp;
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE write_pipe;
  REQUIRE(CreatePipe(&sp.read_pipe, &write_pipe, &sa, 0));
  SetHandleInformation(sp.read_pipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOA si{};
  si.cb = sizeof(si);
  si.dwFlags |= STARTF_USESTDHANDLES;
  si.hStdOutput = write_pipe;
  si.hStdError = write_pipe;

  // Port 0: OS picks an unused port, printed back on the server's first
  // stdout line (see tools/lsmdb_server.cpp) for read_line() to parse below.
  std::string cmdline = "\"" + server_path + "\" \"" + dir + "\" 0";
  std::vector<char> cmdline_buf(cmdline.begin(), cmdline.end());
  cmdline_buf.push_back('\0');

  BOOL ok = CreateProcessA(nullptr, cmdline_buf.data(), nullptr, nullptr, TRUE, 0, nullptr,
                            nullptr, &si, &sp.pi);
  CloseHandle(write_pipe);
  REQUIRE(ok);
  return sp;
}

std::string read_line(SpawnedServer& sp) {
  std::string line;
  char c;
  DWORD bytes_read;
  while (ReadFile(sp.read_pipe, &c, 1, &bytes_read, nullptr) && bytes_read == 1) {
    if (c == '\n') break;
    if (c != '\r') line.push_back(c);
  }
  return line;
}

void hard_kill(SpawnedServer& sp) {
  TerminateProcess(sp.pi.hProcess, 1);
  WaitForSingleObject(sp.pi.hProcess, INFINITE);
  CloseHandle(sp.read_pipe);
  CloseHandle(sp.pi.hProcess);
  CloseHandle(sp.pi.hThread);
}

#else

struct SpawnedServer {
  pid_t pid = -1;
  int read_fd = -1;
};

SpawnedServer spawn_server(const std::string& server_path, const std::string& dir) {
  int fds[2];
  REQUIRE(pipe(fds) == 0);

  pid_t pid = fork();
  REQUIRE(pid >= 0);

  if (pid == 0) {
    dup2(fds[1], STDOUT_FILENO);
    close(fds[0]);
    close(fds[1]);
    execl(server_path.c_str(), server_path.c_str(), dir.c_str(), "0", static_cast<char*>(nullptr));
    _exit(127);  // only reached if execl itself failed
  }

  close(fds[1]);
  return SpawnedServer{pid, fds[0]};
}

std::string read_line(SpawnedServer& sp) {
  std::string line;
  char c;
  while (read(sp.read_fd, &c, 1) == 1) {
    if (c == '\n') break;
    if (c != '\r') line.push_back(c);
  }
  return line;
}

void hard_kill(SpawnedServer& sp) {
  kill(sp.pid, SIGKILL);
  int status;
  waitpid(sp.pid, &status, 0);
  close(sp.read_fd);
}

#endif

// Expects the exact "LISTENING <port>" line tools/lsmdb_server.cpp prints as
// its first line of stdout.
int parse_listening_port(const std::string& line) {
  auto space = line.find(' ');
  REQUIRE(space != std::string::npos);
  return std::stoi(line.substr(space + 1));
}

}  // namespace

TEST_CASE(
    "data PUT through the real network protocol survives the live server process being "
    "hard-killed, and is readable again from a freshly restarted server",
    "[server][crash]") {
  auto dir = make_temp_dir();

  auto server1 = spawn_server(LSMDB_SERVER_PATH, dir.string());
  int port1 = parse_listening_port(read_line(server1));

  {
    asio::io_context client_io;
    asio::ip::tcp::socket socket(client_io);
    asio::ip::tcp::resolver resolver(client_io);
    asio::connect(socket, resolver.resolve("127.0.0.1", std::to_string(port1)));

    for (int i = 0; i < 10; ++i) {
      auto response = send_command(
          socket, "PUT crash-key-" + std::to_string(i) + " crash-value-" + std::to_string(i));
      REQUIRE(response == "+OK");
    }
  }  // socket closes here -- every PUT above already got its +OK, which is
     // exactly the acknowledged-write guarantee being tested; nothing about
     // this test depends on what happens to writes that were never
     // acknowledged.

  hard_kill(server1);  // no graceful shutdown, no chance for any flush
                        // beyond what put() already did synchronously

  // Restart on a fresh OS-assigned port rather than reusing port1 -- some
  // platforms hold a just-killed listening socket's port briefly
  // unavailable (TIME_WAIT-adjacent behavior), and "restart the server"
  // doesn't require reusing the exact same port to be a meaningful recovery
  // demonstration.
  auto server2 = spawn_server(LSMDB_SERVER_PATH, dir.string());
  int port2 = parse_listening_port(read_line(server2));

  {
    asio::io_context client_io;
    asio::ip::tcp::socket socket(client_io);
    asio::ip::tcp::resolver resolver(client_io);
    asio::connect(socket, resolver.resolve("127.0.0.1", std::to_string(port2)));

    for (int i = 0; i < 10; ++i) {
      auto response = send_command(socket, "GET crash-key-" + std::to_string(i));
      REQUIRE(response == "+crash-value-" + std::to_string(i));
    }
  }

  hard_kill(server2);
  std::filesystem::remove_all(dir);
}
