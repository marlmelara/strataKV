#pragma once

namespace stratakv {

// The engine version, set from the CMake project version at compile time. The
// Python package re-exports this so `stratakv.version()` and the C++ library
// can never disagree about which build is loaded.
//
// Named VersionString rather than Version because `stratakv::Version` is the
// immutable file-set snapshot class in src/version.h. Two entities with one
// name in one namespace only compiled because no translation unit happened to
// include both headers -- which is luck, not design.
const char* VersionString();

}  // namespace stratakv
