// A child process for crash-recovery tests.
//
// Writes keys to a database and prints each one to stdout *after* Put returns
// OK, so whatever the parent reads is exactly the set of acknowledged writes.
// If STRATAKV_CRASH_AT names a crash point, the engine kills this process when
// it reaches that point.
//
// Output is written with raw write(2) rather than printf, because std::_Exit
// does not flush stdio buffers -- buffered output would be lost precisely in
// the case these tests care about.

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include <unistd.h>

#include "stratakv/db.h"
#include "stratakv/options.h"
#include "stratakv/status.h"

namespace {

void Emit(const std::string& line) {
  const std::string out = line + "\n";
  ssize_t written = 0;
  while (written < static_cast<ssize_t>(out.size())) {
    const ssize_t result =
        ::write(1, out.data() + written, out.size() - static_cast<size_t>(written));
    if (result < 0) return;
    written += result;
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s <db_path> <key_count> <memtable_bytes>\n", argv[0]);
    return 2;
  }
  const std::string path = argv[1];
  const int key_count = std::atoi(argv[2]);
  const auto memtable_bytes = static_cast<size_t>(std::atol(argv[3]));

  stratakv::Options options;
  options.memtable_bytes = memtable_bytes;

  std::unique_ptr<stratakv::DB> db;
  if (const stratakv::Status status = stratakv::DB::Open(options, path, &db); !status.ok()) {
    std::fprintf(stderr, "open failed: %s\n", status.ToString().c_str());
    return 3;
  }

  for (int i = 0; i < key_count; ++i) {
    char key[64];
    char value[64];
    std::snprintf(key, sizeof(key), "key-%06d", i);
    std::snprintf(value, sizeof(value), "value-%06d", i);

    if (const stratakv::Status status = db->Put(stratakv::WriteOptions{}, key, value);
        !status.ok()) {
      std::fprintf(stderr, "put failed: %s\n", status.ToString().c_str());
      return 4;
    }
    // Printed only after Put returned OK, so this line *is* the promise.
    Emit(key);
  }

  if (const stratakv::Status status = db->Close(); !status.ok()) {
    std::fprintf(stderr, "close failed: %s\n", status.ToString().c_str());
    return 5;
  }
  return 0;
}
