#pragma once

namespace stratakv {

// The engine version, set from the CMake project version at compile time. The
// Python package re-exports this so `stratakv.version()` and the C++ library
// can never disagree about which build is loaded.
const char* Version();

}  // namespace stratakv
