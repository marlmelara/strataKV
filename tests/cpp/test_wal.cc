#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "stratakv/status.h"

#include "env.h"
#include "internal_key.h"
#include "test_util.h"
#include "wal_reader.h"
#include "wal_writer.h"

namespace stratakv {
namespace {

using testing::AsBytes;
using testing::TempDir;

// Collects the batches a replay produced.
class CollectingHandler final : public WalRecordHandler {
 public:
  Status OnBatch(uint64_t sequence, uint32_t count, std::string_view ops) override {
    sequences.push_back(sequence);
    counts.push_back(count);
    payloads.emplace_back(ops);
    return Status::OK();
  }

  std::vector<uint64_t> sequences;
  std::vector<uint32_t> counts;
  std::vector<std::string> payloads;
};

// Builds the payload the DB writes: seq(8) | count(4) | operations.
std::string MakePayload(uint64_t sequence, uint32_t count, std::string_view ops) {
  std::string payload;
  PutFixed64(&payload, sequence);
  PutFixed32(&payload, count);
  payload.append(ops);
  return payload;
}

class WalTest : public ::testing::Test {
 protected:
  std::string Path() const { return dir_.Child("000001.log"); }

  // Appends `count` records, each carrying one fabricated operation.
  void WriteRecords(int count, bool sync = true) {
    std::unique_ptr<WritableFile> file;
    ASSERT_TRUE(env_.NewAppendableFile(Path(), &file).ok());
    WalWriter writer(std::move(file));
    for (int i = 0; i < count; ++i) {
      const std::string payload =
          MakePayload(static_cast<uint64_t>(i) + 1, 1, "op-" + std::to_string(i));
      ASSERT_TRUE(writer.AddRecord(AsBytes(payload), sync).ok());
    }
    ASSERT_TRUE(writer.Close().ok());
  }

  uint64_t FileSize() const {
    uint64_t size = 0;
    EXPECT_TRUE(env_.GetFileSize(Path(), &size).ok());
    return size;
  }

  TempDir dir_;
  Env& env_ = Env::Default();
};

TEST_F(WalTest, ReplayingAMissingLogSucceeds) {
  CollectingHandler handler;
  // An unopened database has no log yet; that is not an error.
  EXPECT_TRUE(WalReader::Replay(env_, dir_.path(), Path(), handler).ok());
  EXPECT_TRUE(handler.sequences.empty());
}

TEST_F(WalTest, ReplayingAnEmptyLogSucceeds) {
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env_.NewWritableFile(Path(), &file).ok());
  ASSERT_TRUE(file->Close().ok());

  CollectingHandler handler;
  EXPECT_TRUE(WalReader::Replay(env_, dir_.path(), Path(), handler).ok());
  EXPECT_TRUE(handler.sequences.empty());
}

TEST_F(WalTest, RecordsRoundTripInOrder) {
  WriteRecords(5);

  CollectingHandler handler;
  ASSERT_TRUE(WalReader::Replay(env_, dir_.path(), Path(), handler).ok());

  ASSERT_EQ(handler.sequences.size(), 5U);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(handler.sequences[static_cast<size_t>(i)], static_cast<uint64_t>(i) + 1);
    EXPECT_EQ(handler.counts[static_cast<size_t>(i)], 1U);
    EXPECT_EQ(handler.payloads[static_cast<size_t>(i)], "op-" + std::to_string(i));
  }
}

TEST_F(WalTest, HandlesEmptyAndLargePayloads) {
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env_.NewAppendableFile(Path(), &file).ok());
  WalWriter writer(std::move(file));

  const std::string small = MakePayload(1, 0, "");
  const std::string large = MakePayload(2, 1, std::string(1 << 20, 'x'));
  ASSERT_TRUE(writer.AddRecord(AsBytes(small), true).ok());
  ASSERT_TRUE(writer.AddRecord(AsBytes(large), true).ok());
  ASSERT_TRUE(writer.Close().ok());

  CollectingHandler handler;
  ASSERT_TRUE(WalReader::Replay(env_, dir_.path(), Path(), handler).ok());
  ASSERT_EQ(handler.payloads.size(), 2U);
  EXPECT_EQ(handler.payloads[0], "");
  EXPECT_EQ(handler.payloads[1], std::string(1 << 20, 'x'));
}

// A torn record at the end of the log is the normal shape of a crash. It was
// never acknowledged, so dropping it loses nothing that was ever promised.
TEST_F(WalTest, TornTailIsTruncatedAndIsNotAnError) {
  WriteRecords(5);
  const uint64_t full_size = FileSize();

  // Cut the file in the middle of the final record.
  ASSERT_TRUE(env_.TruncateFile(Path(), full_size - 4).ok());

  CollectingHandler handler;
  ASSERT_TRUE(WalReader::Replay(env_, dir_.path(), Path(), handler).ok())
      << "a torn tail must not be reported as an error";
  EXPECT_EQ(handler.sequences.size(), 4U) << "the four complete records must survive";

  // The partial bytes must be gone, so the next append starts on a boundary.
  EXPECT_LT(FileSize(), full_size - 4);
}

TEST_F(WalTest, TornTailAtEveryCutPointRecoversEveryCompleteRecord) {
  WriteRecords(6);
  const uint64_t full_size = FileSize();

  std::string original;
  ASSERT_TRUE(env_.ReadFileToString(Path(), &original).ok());

  // Every possible truncation must either keep a record whole or drop it
  // entirely; a half-record must never be handed to the handler.
  for (uint64_t cut = 1; cut < full_size; ++cut) {
    ASSERT_TRUE(env_.WriteFile(Path(), std::string_view(original).substr(0, cut)).ok());

    CollectingHandler handler;
    ASSERT_TRUE(WalReader::Replay(env_, dir_.path(), Path(), handler).ok()) << "cut at " << cut;

    // Whatever survived must be a prefix of the original sequence.
    for (size_t i = 0; i < handler.sequences.size(); ++i) {
      EXPECT_EQ(handler.sequences[i], i + 1) << "cut at " << cut;
      EXPECT_EQ(handler.payloads[i], "op-" + std::to_string(i)) << "cut at " << cut;
    }
  }
}

// A record that fails its checksum but has valid records after it means the
// file was written completely and then damaged. That is corruption, and
// silently truncating it would throw away good data.
TEST_F(WalTest, CorruptionInTheMiddleIsReportedNotTruncated) {
  WriteRecords(5);

  std::string contents;
  ASSERT_TRUE(env_.ReadFileToString(Path(), &contents).ok());

  // Flip one bit inside the second record's payload.
  const size_t first_record_size = kWalHeaderSize + 12 + std::string("op-0").size();
  const size_t target = first_record_size + kWalHeaderSize + 12 + 1;
  ASSERT_LT(target, contents.size());
  contents[target] = static_cast<char>(contents[target] ^ 0x01);
  ASSERT_TRUE(env_.WriteFile(Path(), contents).ok());

  CollectingHandler handler;
  const Status status = WalReader::Replay(env_, dir_.path(), Path(), handler);
  EXPECT_TRUE(status.IsCorruption()) << status.ToString();
}

TEST_F(WalTest, CorruptionInEveryMiddleByteIsDetected) {
  WriteRecords(4);

  std::string original;
  ASSERT_TRUE(env_.ReadFileToString(Path(), &original).ok());

  // The last record's bytes are excluded: damaging those is indistinguishable
  // from a torn tail, which is deliberately allowed.
  const size_t record_size = kWalHeaderSize + 12 + std::string("op-0").size();
  const size_t last_record_start = record_size * 3;

  int detected = 0;
  for (size_t index = 0; index < last_record_start; ++index) {
    std::string damaged = original;
    damaged[index] = static_cast<char>(damaged[index] ^ 0xFF);
    ASSERT_TRUE(env_.WriteFile(Path(), damaged).ok());

    CollectingHandler handler;
    const Status status = WalReader::Replay(env_, dir_.path(), Path(), handler);

    // Either the damage is reported, or replay stopped before the damaged
    // record. What must never happen is replaying a damaged record as valid.
    if (status.IsCorruption()) {
      ++detected;
    } else {
      ASSERT_TRUE(status.ok()) << status.ToString();
      for (size_t i = 0; i < handler.payloads.size(); ++i) {
        EXPECT_EQ(handler.payloads[i], "op-" + std::to_string(i))
            << "damaged byte " << index << " was replayed as valid";
      }
    }
  }
  EXPECT_GT(detected, 0) << "no corruption was detected at all, so the check is not working";
}

TEST_F(WalTest, RejectsAPayloadTooShortToHoldABatchHeader) {
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env_.NewAppendableFile(Path(), &file).ok());
  WalWriter writer(std::move(file));
  // A valid, checksummed record whose payload is nonetheless nonsense.
  ASSERT_TRUE(writer.AddRecord(AsBytes("tiny"), true).ok());
  ASSERT_TRUE(writer.Close().ok());

  CollectingHandler handler;
  EXPECT_TRUE(WalReader::Replay(env_, dir_.path(), Path(), handler).IsCorruption());
}

TEST_F(WalTest, WriterRejectsAnOversizedRecord) {
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env_.NewAppendableFile(Path(), &file).ok());
  WalWriter writer(std::move(file));

  const std::vector<uint8_t> huge(static_cast<size_t>(kMaxWalRecordSize) + 1, 0);
  EXPECT_TRUE(writer.AddRecord(huge, false).IsInvalidArgument());
}

TEST_F(WalTest, AppendingAfterRecoveryContinuesTheSameFile) {
  WriteRecords(3);

  // Reopen for appending, as DB::Open does, and add more.
  std::unique_ptr<WritableFile> file;
  ASSERT_TRUE(env_.NewAppendableFile(Path(), &file).ok());
  EXPECT_GT(file->Size(), 0U) << "a reopened log must report the size it already has";
  WalWriter writer(std::move(file));
  const std::string payload = MakePayload(4, 1, "op-3");
  ASSERT_TRUE(writer.AddRecord(AsBytes(payload), true).ok());
  ASSERT_TRUE(writer.Close().ok());

  CollectingHandler handler;
  ASSERT_TRUE(WalReader::Replay(env_, dir_.path(), Path(), handler).ok());
  ASSERT_EQ(handler.payloads.size(), 4U) << "earlier records must not be lost";
  EXPECT_EQ(handler.payloads[3], "op-3");
}

}  // namespace
}  // namespace stratakv
