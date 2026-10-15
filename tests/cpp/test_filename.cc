#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include "filename.h"

namespace stratakv {
namespace {

TEST(FileName, BuildsZeroPaddedNames) {
  EXPECT_EQ(WalFileName("/db", 1), "/db/000001.log");
  EXPECT_EQ(WalFileName("/db", 999999), "/db/999999.log");
  EXPECT_EQ(TableFileName("/db", 7), "/db/000007.sst");
  EXPECT_EQ(LockFileName("/db"), "/db/LOCK");
}

TEST(FileName, PaddingStopsAtSixDigitsWithoutTruncating) {
  // Beyond six digits the name simply gets longer; it must never be cut short,
  // which would make two different files share a name.
  EXPECT_EQ(TableFileName("/db", 1234567), "/db/1234567.sst");
  EXPECT_EQ(TableFileName("/db", 0), "/db/000000.sst");
}

TEST(FileName, RoundTripsEveryNameItBuilds) {
  for (const uint64_t number : {uint64_t{0}, uint64_t{1}, uint64_t{9}, uint64_t{999999},
                                uint64_t{1000000}, uint64_t{12345678901234ULL}}) {
    for (const FileType expected_type : {FileType::kWal, FileType::kTable}) {
      const std::string full = expected_type == FileType::kWal ? WalFileName("/db", number)
                                                               : TableFileName("/db", number);
      // ParseFileName takes a bare name, as a directory listing provides.
      const std::string bare = full.substr(full.rfind('/') + 1);

      uint64_t parsed = 0;
      FileType type{};
      ASSERT_TRUE(ParseFileName(bare, &parsed, &type)) << bare;
      EXPECT_EQ(parsed, number) << bare;
      EXPECT_EQ(type == FileType::kWal, expected_type == FileType::kWal) << bare;
    }
  }
}

// Anything unrecognised must be ignored rather than misread: a stray file in
// the database directory should not become a phantom table or log.
TEST(FileName, RejectsNamesThatAreNotOurs) {
  uint64_t number = 0;
  FileType type{};

  for (const char* name :
       {"LOCK",      "",       "notes.txt", "README.md", ".DS_Store", "000001",  "000001.sstx",
        "000001.lo", ".log",   ".sst",      "log",       "sst",       "abc.log", "12a4.sst",
        "00-01.log", " 1.log", "1 .sst",    "+1.log",    "-1.sst",    "1.5.log"}) {
    EXPECT_FALSE(ParseFileName(name, &number, &type)) << "accepted " << name;
  }
}

// The digits come from a directory listing, so an absurd name must not overflow
// into a small number that then collides with a real file.
TEST(FileName, RejectsNumbersThatWouldOverflow) {
  uint64_t number = 0;
  FileType type{};

  // 20 nines is larger than UINT64_MAX (about 1.8e19).
  EXPECT_FALSE(ParseFileName("99999999999999999999.log", &number, &type));
  // 21 digits is over the length guard outright.
  EXPECT_FALSE(ParseFileName("999999999999999999999.sst", &number, &type));

  // The largest value that genuinely fits must still be accepted.
  const std::string largest = std::to_string(UINT64_MAX) + ".log";
  EXPECT_TRUE(ParseFileName(largest, &number, &type));
  EXPECT_EQ(number, UINT64_MAX);
}

TEST(FileName, DistinguishesLogsFromTables) {
  uint64_t number = 0;
  FileType type{};

  ASSERT_TRUE(ParseFileName("000042.log", &number, &type));
  EXPECT_EQ(number, 42U);
  EXPECT_TRUE(type == FileType::kWal);

  ASSERT_TRUE(ParseFileName("000042.sst", &number, &type));
  EXPECT_EQ(number, 42U);
  EXPECT_TRUE(type == FileType::kTable);
}

}  // namespace
}  // namespace stratakv
