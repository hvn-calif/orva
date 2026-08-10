#include "avro_compare.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/types/span.h"
#include "avro_bridge.h"

#include <avro/DataFile.hh>
#include <avro/Generic.hh>
#include <avro/GenericDatum.hh>
#include <avro/Stream.hh>
#include <avro/ValidSchema.hh>

namespace security::avro_compare {

namespace bridge = security::avro;

namespace {

// Odd on purpose: boundaries land mid-varint, mid-block-header and
// mid-sync-marker rather than on block alignment.
constexpr size_t kStreamChunkBytes = 4099;

// Private copy: the benchmark has its own, sized for its timed
// stream_read_64k row rather than for hitting awkward split points.
class ChunkedStream final : public bridge::ZeroCopyInputStream {
 public:
  ChunkedStream(const std::string& data, size_t chunk_size)
      : data_(data), chunk_size_(chunk_size) {}

  bool Next(const void** data, int* size) override {
    if (pos_ >= data_.size()) return false;
    size_t n = std::min<size_t>(chunk_size_, data_.size() - pos_);
    *data = data_.data() + pos_;
    *size = static_cast<int>(n);
    pos_ += n;
    return true;
  }
  void BackUp(int count) override { pos_ -= static_cast<size_t>(count); }
  int64_t ByteCount() const override { return static_cast<int64_t>(pos_); }

 private:
  const std::string& data_;
  size_t chunk_size_;
  size_t pos_ = 0;
};

// Value::String and Value::Bytes both keep their payload as raw bytes, just
// under different accessors, so the type predicate that already told us
// which one we are looking at also tells us which accessor to call.
absl::StatusOr<std::string> RawTextBytes(const bridge::AvroValue& value) {
  return value.IsString() ? value.GetString() : value.GetBytes();
}

bool RecordsEqual(const bridge::AvroValue& a, const bridge::AvroValue& b) {
  auto a_fields = a.GetRecordFieldNames();
  auto b_fields = b.GetRecordFieldNames();
  if (!a_fields.ok() || !b_fields.ok() || *a_fields != *b_fields) return false;
  for (const std::string& name : *a_fields) {
    auto a_field = a.GetRecordField(name);
    auto b_field = b.GetRecordField(name);
    if (!a_field.ok() || !b_field.ok()) return false;
    if (!ValuesEqual(*a_field, *b_field)) return false;
  }
  return true;
}

bool ArraysEqual(const bridge::AvroValue& a, const bridge::AvroValue& b) {
  auto a_len = a.GetArrayLen();
  auto b_len = b.GetArrayLen();
  if (!a_len.ok() || !b_len.ok() || *a_len != *b_len) return false;
  for (size_t i = 0; i < *a_len; ++i) {
    auto a_item = a.GetArrayItem(i);
    auto b_item = b.GetArrayItem(i);
    if (!a_item.ok() || !b_item.ok()) return false;
    if (!ValuesEqual(*a_item, *b_item)) return false;
  }
  return true;
}

bool MapsEqual(const bridge::AvroValue& a, const bridge::AvroValue& b) {
  auto a_keys = a.GetMapKeys();
  auto b_keys = b.GetMapKeys();
  if (!a_keys.ok() || !b_keys.ok() || *a_keys != *b_keys) return false;
  for (const std::string& key : *a_keys) {
    auto a_value = a.GetMapValue(key);
    auto b_value = b.GetMapValue(key);
    if (!a_value.ok() || !b_value.ok()) return false;
    if (!ValuesEqual(*a_value, *b_value)) return false;
  }
  return true;
}

bool UnionsEqual(const bridge::AvroValue& a, const bridge::AvroValue& b) {
  auto a_branch = a.GetUnionBranch();
  auto b_branch = b.GetUnionBranch();
  if (!a_branch.ok() || !b_branch.ok() || *a_branch != *b_branch) return false;
  auto a_value = a.GetUnionValue();
  auto b_value = b.GetUnionValue();
  if (!a_value.ok() || !b_value.ok()) return false;
  return ValuesEqual(*a_value, *b_value);
}

}  // namespace

bool ValuesEqual(const bridge::AvroValue& a, const bridge::AvroValue& b) {
  const bool a_is_text = a.IsString() || a.IsBytes();
  const bool b_is_text = b.IsString() || b.IsBytes();
  if (a_is_text && b_is_text) {
    auto a_bytes = RawTextBytes(a);
    auto b_bytes = RawTextBytes(b);
    return a_bytes.ok() && b_bytes.ok() && *a_bytes == *b_bytes;
  }

  if (a.IsRecord() && b.IsRecord()) return RecordsEqual(a, b);
  if (a.IsArray() && b.IsArray()) return ArraysEqual(a, b);
  if (a.IsMap() && b.IsMap()) return MapsEqual(a, b);
  if (a.IsUnion() && b.IsUnion()) return UnionsEqual(a, b);

  // Every other case -- booleans, numbers, enums, fixed, logical types, or
  // a plain type mismatch -- is a leaf whose own equality already does the
  // right thing.
  return a == b;
}

CompareResult CrossReadCircle(const bridge::AvroSchema& ours_schema,
                              const ::avro::ValidSchema& cpp_schema,
                              absl::Span<const bridge::AvroValue> values,
                              bridge::Codec ours_codec,
                              ::avro::Codec cpp_codec) {
  // Ours writes the sample.
  auto writer = bridge::DataFileWriter::Create(ours_schema, ours_codec);
  if (!writer.ok()) return CompareResult::Diverged("our writer create failed");
  for (const bridge::AvroValue& value : values) {
    if (!writer->Append(value).ok()) {
      return CompareResult::Diverged("our append failed");
    }
  }
  auto ours_file = writer->Finish();
  if (!ours_file.ok()) return CompareResult::Diverged("our writer finish failed");

  // avrocpp reads our file and re-writes it with its own writer.
  std::vector<::avro::GenericDatum> relayed;
  {
    auto in = ::avro::memoryInputStream(
        reinterpret_cast<const uint8_t*>(ours_file->data()),
        ours_file->size());
    ::avro::DataFileReader<::avro::GenericDatum> reader(std::move(in));
    ::avro::GenericDatum datum(reader.dataSchema());
    while (reader.read(datum)) relayed.push_back(datum);
  }
  if (relayed.size() != values.size()) {
    return CompareResult::Diverged(
        "avrocpp read a different value count from our file");
  }
  std::unique_ptr<::avro::OutputStream> out = ::avro::memoryOutputStream();
  ::avro::OutputStream* raw = out.get();
  ::avro::DataFileWriter<::avro::GenericDatum> cpp_writer(
      std::move(out), cpp_schema, 16 * 1024, cpp_codec);
  for (const ::avro::GenericDatum& datum : relayed) cpp_writer.write(datum);
  // flush() completes the file (writes the pending block and sync marker);
  // snapshot must happen before close(), which destroys the owned stream.
  cpp_writer.flush();
  auto snapshot = ::avro::snapshot(*raw);
  cpp_writer.close();
  std::string cpp_file(snapshot->begin(), snapshot->end());

  // Ours reads avrocpp's file; values must equal the originals.
  auto reader = bridge::DataFileReader::FromBytes(cpp_file);
  if (!reader.ok()) {
    return CompareResult::Diverged("our reader rejected avrocpp's file");
  }
  for (size_t i = 0; i < values.size(); ++i) {
    auto value = reader->NextValue();
    if (!value.ok()) {
      return CompareResult::Diverged("our read of avrocpp's file failed");
    }
    if (!ValuesEqual(*value, values[i])) {
      return CompareResult::Diverged(
          absl::StrCat("value ", i, " differs after the cross-read circle"));
    }
  }
  auto trailing = reader->NextReady();
  if (!trailing.ok()) {
    return CompareResult::Diverged("reader failed after the last value");
  }
  if (*trailing) return CompareResult::Diverged("trailing values after the circle");

  // The chunked push path timed by stream_read_64k must agree too. An odd
  // chunk size hits arbitrary split points (mid-varint, mid-block header,
  // mid-marker) rather than block-aligned ones.
  ChunkedStream chunked(cpp_file, kStreamChunkBytes);
  auto stream_reader = bridge::DataFileStreamReader::Create(&chunked);
  if (!stream_reader.ok()) {
    return CompareResult::Diverged("stream reader create failed");
  }
  for (size_t i = 0; i < values.size(); ++i) {
    auto value = stream_reader->NextValue();
    if (!value.ok()) {
      return CompareResult::Diverged("chunked stream read failed");
    }
    if (!ValuesEqual(*value, values[i])) {
      return CompareResult::Diverged(
          absl::StrCat("value ", i, " differs on the chunked stream path"));
    }
  }
  auto more = stream_reader->HasNext();
  if (!more.ok()) {
    return CompareResult::Diverged("stream reader failed after the last value");
  }
  if (*more) {
    return CompareResult::Diverged("trailing values on the chunked stream path");
  }

  return CompareResult::Ok();
}

}  // namespace security::avro_compare
