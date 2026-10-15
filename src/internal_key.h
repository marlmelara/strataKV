#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Encoding helpers and the internal key format.
//
// Every key the engine stores on disk or in a memtable is an *internal* key:
// the user's key followed by an 8-byte trailer holding a 7-byte sequence number
// and a 1-byte type. Ordering is user key ascending, then trailer DESCENDING,
// so the newest version of a key is always found first.
namespace stratakv {

// What an entry means. A deletion is not a removal: it is a newer entry that
// shadows older ones, called a tombstone.
enum class ValueType : uint8_t {
  kTypeDeletion = 0,
  kTypeValue = 1,
};

// Sequence numbers occupy 7 bytes of the trailer, so this is the largest one
// the format can represent.
inline constexpr uint64_t kMaxSequenceNumber = (uint64_t{1} << 56) - 1;

// Bytes of sequence number plus type appended to every user key.
inline constexpr size_t kInternalKeyTrailerSize = 8;

// When seeking for a Get, the trailer is built with the *largest* type value so
// that an entry written at exactly the snapshot sequence is still found. Since
// the trailer sorts descending, (seq << 8 | kTypeValue) is the first candidate.
inline constexpr ValueType kValueTypeForSeek = ValueType::kTypeValue;

// --- Fixed-width little-endian ---

void EncodeFixed32(uint8_t* dst, uint32_t value);
void EncodeFixed64(uint8_t* dst, uint64_t value);
uint32_t DecodeFixed32(const uint8_t* src);
uint64_t DecodeFixed64(const uint8_t* src);

void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);

// --- Varint: small numbers in fewer bytes ---
//
// Each byte carries 7 bits of the number; its top bit says "more bytes follow".
// 1 takes one byte, 300 takes two. Key and value lengths are usually small, so
// this is a real saving over a fixed 4 bytes.

void PutVarint32(std::string* dst, uint32_t value);

// Advances `input` past the bytes consumed. Returns false on a truncated or
// over-long varint: data read from disk is never trusted.
bool GetVarint32(std::string_view* input, uint32_t* value);

// A varint length followed by that many bytes.
void PutLengthPrefixed(std::string* dst, std::string_view value);
bool GetLengthPrefixed(std::string_view* input, std::string_view* result);

// --- Internal keys ---

// Packs the trailer: sequence in the high 7 bytes, type in the low byte.
uint64_t PackSequenceAndType(uint64_t sequence, ValueType type);

// Appends user_key + trailer to dst.
void AppendInternalKey(std::string* dst, std::string_view user_key, uint64_t sequence,
                       ValueType type);

// The user's portion of an internal key.
std::string_view ExtractUserKey(std::string_view internal_key);

// The packed trailer of an internal key.
uint64_t ExtractTrailer(std::string_view internal_key);

ValueType ExtractValueType(std::string_view internal_key);
uint64_t ExtractSequence(std::string_view internal_key);

// Orders internal keys so that, for one user key, the newest version is first.
class InternalKeyComparator {
 public:
  // Negative, zero, or positive, like Python 2's cmp().
  int Compare(std::string_view a, std::string_view b) const;
};

}  // namespace stratakv
