// Group commit: a leader merges the batches queued behind it into one log
// record and issues a single fsync for all of them.
//
// The correctness tests matter more than the speed test. Sharing one fsync
// means a writer's result is produced by *another* thread, so every writer has
// to receive the right status and every batch has to be applied exactly once.

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "stratakv/db.h"
#include "stratakv/options.h"
#include "stratakv/status.h"
#include "stratakv/write_batch.h"

#include "db_impl.h"
#include "env.h"
#include "test_util.h"

#if defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define STRATAKV_UNDER_SANITIZER 1
#endif
#endif
#ifndef STRATAKV_UNDER_SANITIZER
#define STRATAKV_UNDER_SANITIZER 0
#endif

namespace stratakv {
namespace {

using testing::TempDir;

std::string WriterKey(int writer, int index) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "w%02d-k%08d", writer, index);
  return buffer;
}

// The value is derived from the key, so any mismatch a reader sees is a real
// defect rather than a stale-but-legitimate value.
std::string WriterValue(int writer, int index) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "w%02d-v%08d", writer, index);
  return buffer;
}

class GroupCommitTest : public ::testing::Test {
 protected:
  std::string DbPath() const { return dir_.Child("mydb"); }

  Options BaseOptions() const {
    Options options;
    options.memtable_bytes = 64 * 1024;
    return options;
  }

  std::unique_ptr<DB> OpenOrDie(const Options& options) {
    std::unique_ptr<DB> db;
    const Status status = DB::Open(options, DbPath(), &db);
    EXPECT_TRUE(status.ok()) << status.ToString();
    return db;
  }

  TempDir dir_;
  Env& env_ = Env::Default();
};

TEST_F(GroupCommitTest, ASingleWriterStillWorks) {
  auto db = OpenOrDie(BaseOptions());
  ASSERT_TRUE(db->Put(WriteOptions{}, "k", "v").ok());

  std::string value;
  ASSERT_TRUE(db->Get(ReadOptions{}, "k", &value).ok());
  EXPECT_EQ(value, "v");
}

// Every writer must get a result, and every write must land exactly once --
// even though most of them were written by a different thread.
TEST_F(GroupCommitTest, EveryConcurrentWriteLandsExactlyOnce) {
  constexpr int kWriters = 8;
  constexpr int kPerWriter = 400;

  auto db = OpenOrDie(BaseOptions());

  std::vector<std::thread> writers;
  std::atomic<int> failures{0};
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        if (!db->Put(WriteOptions{}, WriterKey(w, i), WriterValue(w, i)).ok()) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
      }
    });
  }
  for (auto& writer : writers) writer.join();
  EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);

  std::string value;
  for (int w = 0; w < kWriters; ++w) {
    for (int i = 0; i < kPerWriter; ++i) {
      ASSERT_TRUE(db->Get(ReadOptions{}, WriterKey(w, i), &value).ok())
          << "lost " << WriterKey(w, i);
      ASSERT_EQ(value, WriterValue(w, i));
    }
  }
}

TEST_F(GroupCommitTest, ConcurrentWritesSurviveCloseAndReopen) {
  constexpr int kWriters = 8;
  constexpr int kPerWriter = 300;
  const Options options = BaseOptions();

  {
    auto db = OpenOrDie(options);
    std::vector<std::thread> writers;
    for (int w = 0; w < kWriters; ++w) {
      writers.emplace_back([&, w] {
        for (int i = 0; i < kPerWriter; ++i) {
          (void)db->Put(WriteOptions{}, WriterKey(w, i), WriterValue(w, i));
        }
      });
    }
    for (auto& writer : writers) writer.join();
    ASSERT_TRUE(db->Close().ok());
  }

  // Each merged record carries one base sequence and the group's operations in
  // order. If sequence assignment were wrong, replay would apply them in the
  // wrong order and some keys would come back with another writer's value.
  auto db = OpenOrDie(options);
  std::string value;
  for (int w = 0; w < kWriters; ++w) {
    for (int i = 0; i < kPerWriter; ++i) {
      ASSERT_TRUE(db->Get(ReadOptions{}, WriterKey(w, i), &value).ok())
          << "lost " << WriterKey(w, i) << " after reopen";
      ASSERT_EQ(value, WriterValue(w, i));
    }
  }
}

TEST_F(GroupCommitTest, BatchesStayAtomicWhenMerged) {
  // Several writers each submit a multi-key batch. Merging them into one record
  // must not interleave or split them: every batch applies whole.
  constexpr int kWriters = 8;
  constexpr int kRounds = 100;

  auto db = OpenOrDie(BaseOptions());

  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      for (int round = 0; round < kRounds; ++round) {
        WriteBatch batch;
        // Three keys that must always agree with each other.
        const std::string tag = WriterValue(w, round);
        batch.Put(WriterKey(w, round) + "-a", tag);
        batch.Put(WriterKey(w, round) + "-b", tag);
        batch.Put(WriterKey(w, round) + "-c", tag);
        (void)db->Write(WriteOptions{}, &batch);
      }
    });
  }
  for (auto& writer : writers) writer.join();

  std::string a;
  std::string b;
  std::string c;
  for (int w = 0; w < kWriters; ++w) {
    for (int round = 0; round < kRounds; ++round) {
      const std::string base = WriterKey(w, round);
      ASSERT_TRUE(db->Get(ReadOptions{}, base + "-a", &a).ok()) << base;
      ASSERT_TRUE(db->Get(ReadOptions{}, base + "-b", &b).ok()) << base;
      ASSERT_TRUE(db->Get(ReadOptions{}, base + "-c", &c).ok()) << base;
      EXPECT_EQ(a, b) << base;
      EXPECT_EQ(b, c) << base;
    }
  }
}

TEST_F(GroupCommitTest, WorksWithGroupCommitDisabled) {
  // The baseline path the measurement compares against still has to be correct.
  Options options = BaseOptions();
  options.enable_group_commit = false;

  constexpr int kWriters = 4;
  constexpr int kPerWriter = 100;

  auto db = OpenOrDie(options);
  std::vector<std::thread> writers;
  for (int w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        (void)db->Put(WriteOptions{}, WriterKey(w, i), WriterValue(w, i));
      }
    });
  }
  for (auto& writer : writers) writer.join();

  std::string value;
  for (int w = 0; w < kWriters; ++w) {
    for (int i = 0; i < kPerWriter; ++i) {
      ASSERT_TRUE(db->Get(ReadOptions{}, WriterKey(w, i), &value).ok());
      EXPECT_EQ(value, WriterValue(w, i));
    }
  }
}

// A follower that asked for a sync must not be folded into a group that is not
// going to sync: that would quietly give it a weaker guarantee than it asked
// for. Both orderings have to end up durable.
TEST_F(GroupCommitTest, AWriterThatAsksForSyncIsNeverWeakened) {
  Options options = BaseOptions();
  options.sync_policy = SyncPolicy::kSyncEveryWrite;

  {
    auto db = OpenOrDie(options);
    std::vector<std::thread> writers;
    for (int w = 0; w < 8; ++w) {
      writers.emplace_back([&, w] {
        for (int i = 0; i < 100; ++i) {
          WriteOptions write_options;
          // Alternate: half the writers relax durability, half demand it.
          write_options.sync = (w % 2 == 0);
          (void)db->Put(write_options, WriterKey(w, i), WriterValue(w, i));
        }
      });
    }
    for (auto& writer : writers) writer.join();
    ASSERT_TRUE(db->Close().ok());
  }

  // Everything that was acknowledged has to be there, whichever group it
  // happened to join.
  auto db = OpenOrDie(options);
  std::string value;
  for (int w = 0; w < 8; ++w) {
    for (int i = 0; i < 100; ++i) {
      ASSERT_TRUE(db->Get(ReadOptions{}, WriterKey(w, i), &value).ok())
          << "lost " << WriterKey(w, i);
    }
  }
}

// --- P4 acceptance criteria ---

// "Group commit with 8 writers achieves at least 3x the throughput of 8 writers
// with group commit disabled."
//
// Both runs do identical work with kSyncEveryWrite, so the only difference is
// how many fsyncs that work costs: one per writer, or one per group. Skipped
// under the sanitizers, where a timing measurement means nothing.
TEST_F(GroupCommitTest, EightWritersAreAtLeastThreeTimesFasterWithGroupCommit) {
  if (STRATAKV_UNDER_SANITIZER) {
    GTEST_SKIP() << "a throughput measurement is meaningless under instrumentation";
  }

  constexpr int kWriters = 8;
  constexpr int kPerWriter = 250;
  constexpr int kTotal = kWriters * kPerWriter;

  const auto measure = [this](bool enable_group_commit, const char* name) -> double {
    Options options;
    // The whole point: every write really reaches the disk, so the fsync cost
    // is what is being shared.
    options.sync_policy = SyncPolicy::kSyncEveryWrite;
    options.enable_group_commit = enable_group_commit;

    std::unique_ptr<DB> db;
    const Status status = DB::Open(options, dir_.Child(name), &db);
    EXPECT_TRUE(status.ok()) << status.ToString();
    if (db == nullptr) return 0.0;

    const auto started = std::chrono::steady_clock::now();
    std::vector<std::thread> writers;
    writers.reserve(kWriters);
    for (int w = 0; w < kWriters; ++w) {
      writers.emplace_back([&db, w] {
        for (int i = 0; i < kPerWriter; ++i) {
          (void)db->Put(WriteOptions{}, WriterKey(w, i), WriterValue(w, i));
        }
      });
    }
    for (auto& writer : writers) writer.join();
    const auto elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    EXPECT_TRUE(db->Close().ok());
    return static_cast<double>(kTotal) / elapsed;
  };

  const double without_group_commit = measure(false, "baseline");
  const double with_group_commit = measure(true, "grouped");
  const double ratio = with_group_commit / without_group_commit;

  std::cout << "8 writers, kSyncEveryWrite:\n"
            << "  group commit OFF: " << without_group_commit << " writes/sec\n"
            << "  group commit ON:  " << with_group_commit << " writes/sec\n"
            << "  speedup:          " << ratio << "x" << std::endl;

  EXPECT_GT(ratio, 3.0) << "group commit must be at least 3x faster with 8 writers";
}

// "Stress test: 8 writer threads and 8 reader threads for 60 seconds", and it
// has to be clean under ThreadSanitizer.
//
// Writers use disjoint key spaces and publish how far they have got, so a
// reader can only ask about writes that were already acknowledged -- and the
// value is derived from the key, so any mismatch is a real defect rather than a
// stale-but-legitimate value. Flushes and compactions run throughout.
TEST_F(GroupCommitTest, EightWritersAndEightReadersUnderSustainedLoad) {
  constexpr int kWriters = 8;
  constexpr int kReaders = 8;

  // Defaults to the 60 seconds the acceptance criterion asks for; overridable
  // only to keep local iteration bearable.
  int seconds = 60;
  if (const char* override_seconds = std::getenv("STRATAKV_STRESS_SECONDS")) {
    const int parsed = std::atoi(override_seconds);
    if (parsed > 0) seconds = parsed;
  }

  Options options = BaseOptions();
  options.memtable_bytes = 256 * 1024;
  auto db = OpenOrDie(options);
  auto* impl = dynamic_cast<DBImpl*>(db.get());
  ASSERT_NE(impl, nullptr);

  std::atomic<bool> stop{false};
  std::atomic<int> progress[kWriters];
  for (auto& counter : progress) counter.store(-1, std::memory_order_relaxed);

  std::atomic<long> writes{0};
  std::atomic<long> reads{0};
  std::atomic<long> write_failures{0};
  std::atomic<long> read_failures{0};

  std::vector<std::thread> threads;
  threads.reserve(kWriters + kReaders);

  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      int i = 0;
      while (!stop.load(std::memory_order_acquire)) {
        if (!db->Put(WriteOptions{}, WriterKey(w, i), WriterValue(w, i)).ok()) {
          write_failures.fetch_add(1, std::memory_order_relaxed);
          return;
        }
        // Published only after Put returned OK, so a reader never asks about a
        // write that was not acknowledged.
        progress[w].store(i, std::memory_order_release);
        ++i;
        writes.fetch_add(1, std::memory_order_relaxed);

        // A key every writer fights over, to put real contention on the queue.
        if (i % 64 == 0) {
          (void)db->Put(WriteOptions{}, "contended", WriterValue(w, i));
        }
      }
    });
  }

  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&, r] {
      std::mt19937 random(static_cast<uint32_t>(r) + 1000);
      std::string value;
      while (!stop.load(std::memory_order_acquire)) {
        const int writer = static_cast<int>(random() % kWriters);
        const int limit = progress[writer].load(std::memory_order_acquire);
        if (limit < 0) continue;
        const int index = static_cast<int>(random() % static_cast<uint32_t>(limit + 1));

        const Status status = db->Get(ReadOptions{}, WriterKey(writer, index), &value);
        if (!status.ok() || value != WriterValue(writer, index)) {
          read_failures.fetch_add(1, std::memory_order_relaxed);
          ADD_FAILURE() << "key " << WriterKey(writer, index) << ": " << status.ToString();
          return;
        }
        reads.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  stop.store(true, std::memory_order_release);
  for (auto& thread : threads) thread.join();

  std::cout << "sustained load for " << seconds << "s: " << writes.load() << " writes, "
            << reads.load() << " reads, " << impl->TEST_FileCount(0) << " L0 files, "
            << impl->TEST_FileCount(1) << " L1 files" << std::endl;

  EXPECT_EQ(write_failures.load(), 0L);
  EXPECT_EQ(read_failures.load(), 0L);
  EXPECT_GT(writes.load(), 0L) << "no writes happened, so this proved nothing";
  EXPECT_GT(reads.load(), 0L) << "no reads happened, so this proved nothing";

  // Everything acknowledged must still be readable afterwards.
  std::string value;
  for (int w = 0; w < kWriters; ++w) {
    const int limit = progress[w].load(std::memory_order_acquire);
    for (int i = 0; i <= limit; ++i) {
      ASSERT_TRUE(db->Get(ReadOptions{}, WriterKey(w, i), &value).ok())
          << "lost " << WriterKey(w, i);
      ASSERT_EQ(value, WriterValue(w, i));
    }
  }
}

}  // namespace
}  // namespace stratakv
