#include <cstring>
#include <regex>
#include <string>

#include <gtest/gtest.h>

#include "stratakv/version.h"

namespace stratakv {
namespace {

// These tests exist because of a real bug. P3 added src/version.cc for the
// VersionSet, which overwrote the P0 file of the same name that defined this
// function. Nothing in the C++ tests referenced it, so the whole suite stayed
// green and only the Python binding failed to load -- a link error that
// surfaced in CI rather than locally.
//
// Referencing it from a C++ test means the next time its definition goes
// missing, the C++ build fails immediately instead of the failure hiding until
// something tries to import the module.

TEST(VersionString, IsNotEmpty) {
  ASSERT_NE(VersionString(), nullptr);
  EXPECT_GT(std::strlen(VersionString()), 0U);
}

TEST(VersionString, LooksLikeAReleaseNumber) {
  const std::string version = VersionString();
  EXPECT_TRUE(std::regex_match(version, std::regex(R"(\d+\.\d+\.\d+)"))) << version;
}

TEST(VersionString, IsStableAcrossCalls) { EXPECT_STREQ(VersionString(), VersionString()); }

TEST(VersionString, CameFromTheBuildRatherThanTheFallback) {
  // The fallback only appears when STRATAKV_VERSION was never defined, which
  // would mean the CMake project version stopped reaching the compiler.
  EXPECT_STRNE(VersionString(), "0.0.0-unknown");
}

}  // namespace
}  // namespace stratakv
