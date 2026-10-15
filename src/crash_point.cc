#include "crash_point.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace stratakv::crash {
namespace {

// Atomic so a test may set it from one thread while the engine reads it from
// another without a data race.
std::atomic<const char*>& OverridePoint() {
  static std::atomic<const char*> point{nullptr};
  return point;
}

// A function-local static is initialized once, on first call, thread-safely.
// Reading the environment once also means a test cannot change it midway and
// get confusing results.
const char* EnvironmentPoint() {
  static const char* value = std::getenv("STRATAKV_CRASH_AT");
  return value;
}

}  // namespace

bool IsArmed(const char* name) {
  const char* armed = OverridePoint().load(std::memory_order_acquire);
  if (armed == nullptr) armed = EnvironmentPoint();
  return armed != nullptr && std::strcmp(armed, name) == 0;
}

void CrashPoint(const char* name) {
  if (IsArmed(name)) {
    // _Exit, not exit: it skips destructors and does not flush buffers, which
    // is what makes this imitate a real crash instead of a clean shutdown.
    std::_Exit(1);
  }
}

void SetArmedPointForTesting(const char* name) {
  OverridePoint().store(name, std::memory_order_release);
}

}  // namespace stratakv::crash
