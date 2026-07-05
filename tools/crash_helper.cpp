// Test-only helper binary, not part of the library. Opens a Db, writes a
// fixed number of keys, then spins forever without exiting cleanly -- the
// test harness (tests/core/crash_recovery_test.cpp) hard-kills this process
// (SIGKILL / TerminateProcess) after giving the writes time to complete, then
// opens a fresh Db on the same directory and checks that everything survived.
//
// The point of doing this via a real separate OS process, rather than
// simulating a "crash" inside the test binary itself, is that it's the only
// way to actually exercise what happens when the OS forcibly terminates a
// process -- no destructors run, no atexit handlers fire, nothing gets a
// chance to do a "clean" shutdown. That's a meaningfully different, stronger
// claim than "the code behaves correctly if you call the destructor," and
// it's the literal scenario a crash-recovery guarantee is supposed to cover.
#include "lsmdb/db.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: crash_helper <db_dir> <num_keys>\n";
    return 1;
  }
  std::string dir = argv[1];
  int num_keys = std::stoi(argv[2]);

  // A huge flush threshold and compaction threshold so this test exercises
  // WAL replay specifically -- the data must still be sitting in the
  // memtable (never flushed to an SSTable) when the process is killed, since
  // that's the scenario WAL replay exists to cover.
  lsmdb::Db db(dir, /*memtable_flush_threshold_bytes=*/1024ull * 1024 * 1024,
               /*compaction_sstable_threshold=*/1000);

  for (int i = 0; i < num_keys; ++i) {
    db.put("crash-key-" + std::to_string(i), "crash-value-" + std::to_string(i));
  }
  std::cout << "all-writes-complete" << std::endl;  // flushed -- the parent
                                                      // waits to read this line
                                                      // before it kills us

  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}
