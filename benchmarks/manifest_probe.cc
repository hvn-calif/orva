// Where the time goes when decoding an Apache Iceberg v2 manifest.
//
// The other probes here use shapes we invented (flat, nested, strings). A
// real Iceberg manifest is a different beast, and the difference is the whole
// point: its per-entry metrics are `map<int, long>` and `map<int, binary>`,
// and Avro map keys must be strings, so Iceberg encodes them as
// `array<record{key, value}>` (schema_conversion.py in iceberg-python; see
// "logicalType": "map" below). A table with N columns therefore puts 6*N
// two-field sub-records inside every single manifest entry.
//
// That matters because apache-avro represents a record as
// `Vec<(String, Value)>` (rust/value.rs). Every one of those 6*N sub-records
// heap-allocates two Strings for the literal field names "key" and "value",
// on top of the Vec and the Value enum for each leaf. avro-cpp's
// GenericRecord keeps field names in the shared schema node and stores only
// the values, so it allocates nothing per field name.
//
// Rows, per entry:
//   decode_only     decode, touch nothing. The AST-build cost alone.
//   ours_reuse/*    the same full-schema decode/walk, but overwrite one
//                   caller-owned AvroValue as avro-cpp overwrites one
//                   GenericDatum.
//   decode_planner  decode + read what a query planner actually reads:
//                   status, file_path, record_count, file_size_in_bytes, and
//                   the lower/upper bound of one column (predicate pushdown).
//   decode_full     decode + read every metrics entry.
//   project_decode  decode through a projected *reader* schema that keeps
//                   only status + file_path + record_count +
//                   file_size_in_bytes. Schema resolution is allowed to skip
//                   the rest at the byte level, so this row is a measured
//                   ceiling for what projection pushdown could buy -- no new
//                   decoder needed to find out.
//
// Set MANIFEST_ENTRIES to change the entry count (default 2000). The
// ~150 MB case is MANIFEST_ENTRIES=100000; startup prints the file size so
// the extrapolation is not guesswork.
//
// Fidelity note: the real schema carries a "field-id" attribute on every
// field. They are omitted here because avro-cpp's getCustomAttributes
// (impl/Compiler.cc:278) calls stringValue() on every unrecognised key and
// throws on the integer values. Field ids are metadata: the Avro binary
// layout is positional, so their absence changes no bytes and no decode
// cost. The "logicalType": "map" annotations are kept -- avro-cpp degrades
// unknown logical types to NONE (impl/Compiler.cc:365) rather than throwing.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "benchmark/benchmark.h"

#include "avro_bridge.h"

#include <avro/Compiler.hh>
#include <avro/DataFile.hh>
#include <avro/Generic.hh>
#include <avro/GenericDatum.hh>
#include <avro/Stream.hh>
#include <avro/ValidSchema.hh>

namespace bridge = security::avro;

namespace {

// Columns in the table the manifest describes. Drives the size of all six
// metrics maps, so it is the main lever on per-entry allocation count:
// rust/tests/manifest_alloc.rs measures ~26 allocator operations per column
// per entry. Override with MANIFEST_COLUMNS; real Iceberg tables run from a
// handful of columns to a few hundred.
int32_t Columns() {
  static const int32_t columns = [] {
    const char* override_columns = std::getenv("MANIFEST_COLUMNS");
    if (override_columns == nullptr) return 20L;
    const long parsed = std::strtol(override_columns, nullptr, 10);
    return parsed > 0 ? parsed : 20L;
  }();
  return columns;
}
// Iceberg truncates bounds to 16 bytes by default.
constexpr size_t kBoundBytes = 16;
// The column a pushdown predicate filters on.
constexpr int32_t kTargetColumn = 7;
constexpr size_t kSplitOffsetCount = 8;

size_t EntryCount() {
  const char* override_count = std::getenv("MANIFEST_ENTRIES");
  if (override_count == nullptr) return 2000;
  const long parsed = std::strtol(override_count, nullptr, 10);
  return parsed > 0 ? static_cast<size_t>(parsed) : 2000;
}

void Require(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FATAL: %s\n", what);
    std::exit(1);
  }
}

// Iceberg v2 manifest_entry. Record names (r2, r102, k117_v118, ...) follow
// the names Iceberg's own Avro conversion generates, so the shape matches
// what a reader would meet in the wild.
const char* kManifestJson = R"({
"type":"record","name":"manifest_entry","fields":[
 {"name":"status","type":"int"},
 {"name":"snapshot_id","type":["null","long"],"default":null},
 {"name":"sequence_number","type":["null","long"],"default":null},
 {"name":"file_sequence_number","type":["null","long"],"default":null},
 {"name":"data_file","type":{"type":"record","name":"r2","fields":[
  {"name":"content","type":"int"},
  {"name":"file_path","type":"string"},
  {"name":"file_format","type":"string"},
  {"name":"partition","type":{"type":"record","name":"r102","fields":[
   {"name":"dept","type":["null","string"],"default":null},
   {"name":"event_day","type":["null",{"type":"int","logicalType":"date"}],"default":null}]}},
  {"name":"record_count","type":"long"},
  {"name":"file_size_in_bytes","type":"long"},
  {"name":"column_sizes","type":["null",{"type":"array","logicalType":"map",
   "items":{"type":"record","name":"k117_v118","fields":[
    {"name":"key","type":"int"},{"name":"value","type":"long"}]}}],"default":null},
  {"name":"value_counts","type":["null",{"type":"array","logicalType":"map",
   "items":{"type":"record","name":"k119_v120","fields":[
    {"name":"key","type":"int"},{"name":"value","type":"long"}]}}],"default":null},
  {"name":"null_value_counts","type":["null",{"type":"array","logicalType":"map",
   "items":{"type":"record","name":"k121_v122","fields":[
    {"name":"key","type":"int"},{"name":"value","type":"long"}]}}],"default":null},
  {"name":"nan_value_counts","type":["null",{"type":"array","logicalType":"map",
   "items":{"type":"record","name":"k138_v139","fields":[
    {"name":"key","type":"int"},{"name":"value","type":"long"}]}}],"default":null},
  {"name":"lower_bounds","type":["null",{"type":"array","logicalType":"map",
   "items":{"type":"record","name":"k126_v127","fields":[
    {"name":"key","type":"int"},{"name":"value","type":"bytes"}]}}],"default":null},
  {"name":"upper_bounds","type":["null",{"type":"array","logicalType":"map",
   "items":{"type":"record","name":"k129_v130","fields":[
    {"name":"key","type":"int"},{"name":"value","type":"bytes"}]}}],"default":null},
  {"name":"key_metadata","type":["null","bytes"],"default":null},
  {"name":"split_offsets","type":["null",{"type":"array","items":"long"}],"default":null},
  {"name":"equality_ids","type":["null",{"type":"array","items":"int"}],"default":null},
  {"name":"sort_order_id","type":["null","int"],"default":null}]}}]})";

// The planner's projection: everything needed to list and size files, with
// every metrics map dropped. Record names must match the writer schema for
// Avro resolution to pair the records up.
const char* kProjectionJson = R"({
"type":"record","name":"manifest_entry","fields":[
 {"name":"status","type":"int"},
 {"name":"data_file","type":{"type":"record","name":"r2","fields":[
  {"name":"file_path","type":"string"},
  {"name":"record_count","type":"long"},
  {"name":"file_size_in_bytes","type":"long"}]}}]})";

// Field indices. Positional access keeps the walks honest: both libraries
// reach the same leaf without a name lookup on the avrocpp side.
enum EntryField { kStatus = 0, kSnapshotId = 1, kDataFile = 4 };
enum DataFileField {
  kContent = 0,
  kFilePath = 1,
  kFileFormat = 2,
  kPartition = 3,
  kRecordCount = 4,
  kFileSize = 5,
  kColumnSizes = 6,
  kValueCounts = 7,
  kNullValueCounts = 8,
  kNanValueCounts = 9,
  kLowerBounds = 10,
  kUpperBounds = 11,
  kKeyMetadata = 12,
  kSplitOffsets = 13,
  kEqualityIds = 14,
  kSortOrderId = 15,
};

// ---------------------------------------------------------------------------
// Building the dataset (avrocpp side, as the other probes do).
// ---------------------------------------------------------------------------

// Fills an optional array<record{key,value}> field with one entry per column.
void FillMetricsMap(avro::GenericDatum& field, bool bytes_value, size_t seed) {
  field.selectBranch(1);
  auto& array = field.value<avro::GenericArray>();
  const avro::NodePtr item_node = array.schema()->leafAt(0);
  for (int32_t column = 0; column < Columns(); ++column) {
    avro::GenericDatum item(item_node);
    auto& pair = item.value<avro::GenericRecord>();
    pair.fieldAt(0).value<int32_t>() = column;
    if (bytes_value) {
      std::vector<uint8_t> bound(kBoundBytes);
      for (size_t b = 0; b < kBoundBytes; ++b) {
        bound[b] = static_cast<uint8_t>(seed + column + b);
      }
      pair.fieldAt(1).value<std::vector<uint8_t>>() = std::move(bound);
    } else {
      pair.fieldAt(1).value<int64_t>() =
          static_cast<int64_t>(seed + column * 31);
    }
    array.value().push_back(std::move(item));
  }
}

void BuildEntry(const avro::ValidSchema& schema, size_t i,
                std::vector<avro::GenericDatum>& out) {
  avro::GenericDatum datum(schema.root());
  auto& entry = datum.value<avro::GenericRecord>();

  entry.fieldAt(kStatus).value<int32_t>() = 1;  // ADDED
  entry.fieldAt(kSnapshotId).selectBranch(1);
  entry.fieldAt(kSnapshotId).value<int64_t>() = 7856392845000000000LL;
  entry.fieldAt(2).selectBranch(1);
  entry.fieldAt(2).value<int64_t>() = static_cast<int64_t>(i / 512);
  entry.fieldAt(3).selectBranch(1);
  entry.fieldAt(3).value<int64_t>() = static_cast<int64_t>(i / 512);

  auto& file = entry.fieldAt(kDataFile).value<avro::GenericRecord>();
  file.fieldAt(kContent).value<int32_t>() = 0;  // DATA
  // A realistic warehouse URI; path length is a real part of the payload.
  file.fieldAt(kFilePath).value<std::string>() =
      "s3://warehouse-prod-us-east-1/lakehouse/analytics/events/data/"
      "dept=engineering/event_day=2026-07-" +
      std::to_string(1 + i % 28) + "/00000-" + std::to_string(i) +
      "-8f3c1d92-4b7a-4e51-9c2f-6d0a1e77b3c4-00001.parquet";
  file.fieldAt(kFileFormat).value<std::string>() = "parquet";

  auto& partition = file.fieldAt(kPartition).value<avro::GenericRecord>();
  partition.fieldAt(0).selectBranch(1);
  partition.fieldAt(0).value<std::string>() = "engineering";
  partition.fieldAt(1).selectBranch(1);
  partition.fieldAt(1).value<int32_t>() = 20630 + static_cast<int32_t>(i % 28);

  file.fieldAt(kRecordCount).value<int64_t>() =
      static_cast<int64_t>(100000 + i);
  file.fieldAt(kFileSize).value<int64_t>() =
      static_cast<int64_t>(52428800 + i * 977);

  FillMetricsMap(file.fieldAt(kColumnSizes), false, i);
  FillMetricsMap(file.fieldAt(kValueCounts), false, i + 1);
  FillMetricsMap(file.fieldAt(kNullValueCounts), false, i + 2);
  FillMetricsMap(file.fieldAt(kNanValueCounts), false, i + 3);
  FillMetricsMap(file.fieldAt(kLowerBounds), true, i);
  FillMetricsMap(file.fieldAt(kUpperBounds), true, i + 128);

  // key_metadata and equality_ids stay null: unencrypted data files carry
  // neither, which is the common case.
  auto& offsets = file.fieldAt(kSplitOffsets);
  offsets.selectBranch(1);
  auto& offset_array = offsets.value<avro::GenericArray>();
  for (size_t s = 0; s < kSplitOffsetCount; ++s) {
    offset_array.value().push_back(
        avro::GenericDatum(static_cast<int64_t>(4 + s * 6553600)));
  }
  file.fieldAt(kSortOrderId).selectBranch(1);
  file.fieldAt(kSortOrderId).value<int32_t>() = 0;

  out.push_back(std::move(datum));
}

std::string WriteCpp(const avro::ValidSchema& schema,
                     const std::vector<avro::GenericDatum>& values) {
  std::unique_ptr<avro::OutputStream> out = avro::memoryOutputStream();
  avro::OutputStream* raw = out.get();
  avro::DataFileWriter<avro::GenericDatum> writer(std::move(out), schema,
                                                  64 * 1024,
                                                  avro::NULL_CODEC);
  for (const avro::GenericDatum& value : values) writer.write(value);
  writer.flush();
  auto snapshot = avro::snapshot(*raw);
  writer.close();
  return std::string(snapshot->begin(), snapshot->end());
}

// ---------------------------------------------------------------------------
// Walks. Both sides read exactly the same leaves and return the same
// checksum, which startup asserts.
// ---------------------------------------------------------------------------

bool IsAbsent(const avro::GenericDatum& field) {
  return field.isUnion() && field.unionBranch() == 0;
}

int64_t CppBoundSize(const avro::GenericDatum& field, int32_t column) {
  if (IsAbsent(field)) return 0;
  for (const avro::GenericDatum& item :
       field.value<avro::GenericArray>().value()) {
    const auto& pair = item.value<avro::GenericRecord>();
    if (pair.fieldAt(0).value<int32_t>() == column) {
      return static_cast<int64_t>(
          pair.fieldAt(1).value<std::vector<uint8_t>>().size());
    }
  }
  return 0;
}

int64_t CppSumMetrics(const avro::GenericDatum& field, bool bytes_value) {
  if (IsAbsent(field)) return 0;
  int64_t sum = 0;
  for (const avro::GenericDatum& item :
       field.value<avro::GenericArray>().value()) {
    const auto& pair = item.value<avro::GenericRecord>();
    sum += pair.fieldAt(0).value<int32_t>();
    sum += bytes_value ? static_cast<int64_t>(
                             pair.fieldAt(1).value<std::vector<uint8_t>>().size())
                       : pair.fieldAt(1).value<int64_t>();
  }
  return sum;
}

int64_t CppPlannerWalk(const avro::GenericDatum& datum) {
  const auto& entry = datum.value<avro::GenericRecord>();
  int64_t sum = entry.fieldAt(kStatus).value<int32_t>();
  const auto& file = entry.fieldAt(kDataFile).value<avro::GenericRecord>();
  sum += static_cast<int64_t>(
      file.fieldAt(kFilePath).value<std::string>().size());
  sum += file.fieldAt(kRecordCount).value<int64_t>();
  sum += file.fieldAt(kFileSize).value<int64_t>();
  sum += CppBoundSize(file.fieldAt(kLowerBounds), kTargetColumn);
  sum += CppBoundSize(file.fieldAt(kUpperBounds), kTargetColumn);
  return sum;
}

int64_t CppFullWalk(const avro::GenericDatum& datum) {
  const auto& entry = datum.value<avro::GenericRecord>();
  int64_t sum = entry.fieldAt(kStatus).value<int32_t>();
  const auto& file = entry.fieldAt(kDataFile).value<avro::GenericRecord>();
  sum += static_cast<int64_t>(
      file.fieldAt(kFilePath).value<std::string>().size());
  sum += file.fieldAt(kRecordCount).value<int64_t>();
  sum += file.fieldAt(kFileSize).value<int64_t>();
  sum += CppSumMetrics(file.fieldAt(kColumnSizes), false);
  sum += CppSumMetrics(file.fieldAt(kValueCounts), false);
  sum += CppSumMetrics(file.fieldAt(kNullValueCounts), false);
  sum += CppSumMetrics(file.fieldAt(kNanValueCounts), false);
  sum += CppSumMetrics(file.fieldAt(kLowerBounds), true);
  sum += CppSumMetrics(file.fieldAt(kUpperBounds), true);
  if (!IsAbsent(file.fieldAt(kSplitOffsets))) {
    for (const avro::GenericDatum& offset :
         file.fieldAt(kSplitOffsets).value<avro::GenericArray>().value()) {
      sum += offset.value<int64_t>();
    }
  }
  return sum;
}

int64_t CppProjectionWalk(const avro::GenericDatum& datum) {
  const auto& entry = datum.value<avro::GenericRecord>();
  int64_t sum = entry.fieldAt(0).value<int32_t>();
  const auto& file = entry.fieldAt(1).value<avro::GenericRecord>();
  sum += static_cast<int64_t>(file.fieldAt(0).value<std::string>().size());
  sum += file.fieldAt(1).value<int64_t>();
  sum += file.fieldAt(2).value<int64_t>();
  return sum;
}

// `path` arrives positioned at data_file and leaves unchanged.
int64_t OursBoundSize(const bridge::AvroValue& entry, bridge::AvroPath& path,
                      const char* field, int32_t column) {
  Require(path.PushField(field).ok(), "path: bounds field");
  if (entry.IsNullAt(path).value_or(true)) {
    Require(path.Pop().ok(), "path: pop bounds");
    return 0;
  }
  const uint64_t len = entry.GetArrayLenAt(path).value_or(0);
  int64_t size = 0;
  path.PushIndex(0);
  for (uint64_t i = 0; i < len; ++i) {
    Require(path.SetLastIndex(i).ok(), "path: bounds index");
    Require(path.PushField("key").ok(), "path: bounds key");
    const int32_t key = entry.GetIntAt(path).value_or(-1);
    Require(path.Pop().ok(), "path: pop bounds key");
    if (key != column) continue;
    Require(path.PushField("value").ok(), "path: bounds value");
    auto bound = entry.GetBytesAt(path);
    Require(bound.ok(), "path: bounds value read");
    size = static_cast<int64_t>(bound->size());
    Require(path.Pop().ok(), "path: pop bounds value");
    break;
  }
  Require(path.Pop().ok(), "path: pop bounds index");
  Require(path.Pop().ok(), "path: pop bounds field");
  return size;
}

int64_t OursSumMetrics(const bridge::AvroValue& entry, bridge::AvroPath& path,
                       const char* field, bool bytes_value) {
  Require(path.PushField(field).ok(), "path: metrics field");
  if (entry.IsNullAt(path).value_or(true)) {
    Require(path.Pop().ok(), "path: pop metrics");
    return 0;
  }
  const uint64_t len = entry.GetArrayLenAt(path).value_or(0);
  int64_t sum = 0;
  path.PushIndex(0);
  for (uint64_t i = 0; i < len; ++i) {
    Require(path.SetLastIndex(i).ok(), "path: metrics index");
    Require(path.PushField("key").ok(), "path: metrics key");
    sum += entry.GetIntAt(path).value_or(0);
    Require(path.Pop().ok(), "path: pop metrics key");
    Require(path.PushField("value").ok(), "path: metrics value");
    if (bytes_value) {
      auto value = entry.GetBytesAt(path);
      Require(value.ok(), "path: metrics bytes");
      sum += static_cast<int64_t>(value->size());
    } else {
      sum += entry.GetLongAt(path).value_or(0);
    }
    Require(path.Pop().ok(), "path: pop metrics value");
  }
  Require(path.Pop().ok(), "path: pop metrics index");
  Require(path.Pop().ok(), "path: pop metrics field");
  return sum;
}

// Reads the three scalars every walk shares. Leaves `path` at data_file.
int64_t OursScalars(const bridge::AvroValue& entry, bridge::AvroPath& path) {
  Require(path.PushField("file_path").ok(), "path: file_path");
  auto file_path = entry.GetStringAt(path);
  Require(file_path.ok(), "path: file_path read");
  int64_t sum = static_cast<int64_t>(file_path->size());
  Require(path.Pop().ok(), "path: pop file_path");
  Require(path.PushField("record_count").ok(), "path: record_count");
  sum += entry.GetLongAt(path).value_or(0);
  Require(path.Pop().ok(), "path: pop record_count");
  Require(path.PushField("file_size_in_bytes").ok(), "path: file_size");
  sum += entry.GetLongAt(path).value_or(0);
  Require(path.Pop().ok(), "path: pop file_size");
  return sum;
}

int64_t OursPlannerWalk(const bridge::AvroValue& entry) {
  bridge::AvroPath path;
  Require(path.PushField("status").ok(), "path: status");
  int64_t sum = entry.GetIntAt(path).value_or(0);
  Require(path.Pop().ok(), "path: pop status");

  Require(path.PushField("data_file").ok(), "path: data_file");
  sum += OursScalars(entry, path);
  sum += OursBoundSize(entry, path, "lower_bounds", kTargetColumn);
  sum += OursBoundSize(entry, path, "upper_bounds", kTargetColumn);
  return sum;
}

int64_t OursFullWalk(const bridge::AvroValue& entry) {
  bridge::AvroPath path;
  Require(path.PushField("status").ok(), "path: status");
  int64_t sum = entry.GetIntAt(path).value_or(0);
  Require(path.Pop().ok(), "path: pop status");

  Require(path.PushField("data_file").ok(), "path: data_file");
  sum += OursScalars(entry, path);
  sum += OursSumMetrics(entry, path, "column_sizes", false);
  sum += OursSumMetrics(entry, path, "value_counts", false);
  sum += OursSumMetrics(entry, path, "null_value_counts", false);
  sum += OursSumMetrics(entry, path, "nan_value_counts", false);
  sum += OursSumMetrics(entry, path, "lower_bounds", true);
  sum += OursSumMetrics(entry, path, "upper_bounds", true);

  Require(path.PushField("split_offsets").ok(), "path: split_offsets");
  if (!entry.IsNullAt(path).value_or(true)) {
    const uint64_t len = entry.GetArrayLenAt(path).value_or(0);
    path.PushIndex(0);
    for (uint64_t i = 0; i < len; ++i) {
      Require(path.SetLastIndex(i).ok(), "path: offset index");
      sum += entry.GetLongAt(path).value_or(0);
    }
    Require(path.Pop().ok(), "path: pop offset index");
  }
  Require(path.Pop().ok(), "path: pop split_offsets");
  return sum;
}

int64_t OursProjectionWalk(const bridge::AvroValue& entry) {
  bridge::AvroPath path;
  Require(path.PushField("status").ok(), "path: status");
  int64_t sum = entry.GetIntAt(path).value_or(0);
  Require(path.Pop().ok(), "path: pop status");
  Require(path.PushField("data_file").ok(), "path: data_file");
  return sum + OursScalars(entry, path);
}

// ---------------------------------------------------------------------------
// Readers.
// ---------------------------------------------------------------------------

using OursWalk = int64_t (*)(const bridge::AvroValue&);
using CppWalk = int64_t (*)(const avro::GenericDatum&);

struct Fixture {
  std::string bytes;
  size_t entries = 0;
  // Parsed in main once both libraries have accepted the JSON, so a parse
  // failure reports itself rather than crashing inside a static initializer.
  std::optional<bridge::AvroSchema> projection;
  avro::ValidSchema cpp_projection;
};

Fixture& Data() {
  static Fixture fixture;
  return fixture;
}

// How the projected rows narrow the read on our side.
enum class Mode {
  kFull,        // no projection at all
  kResolve,     // reader-schema resolution (apache-avro; materializes then resolves)
  kProject,     // byte-level projection (crate::decode)
};

int64_t OursRead(OursWalk walk, Mode mode) {
  const Fixture& data = Data();
  auto reader = [&data, mode] {
    switch (mode) {
      case Mode::kResolve:
        return bridge::DataFileReader::FromBytesWithSchema(*data.projection,
                                                           data.bytes);
      case Mode::kProject:
        return bridge::DataFileReader::FromBytesWithProjection(*data.projection,
                                                               data.bytes);
      case Mode::kFull:
        break;
    }
    return bridge::DataFileReader::FromBytes(data.bytes);
  }();
  Require(reader.ok(), "ours: reader construction");
  int64_t sum = 0;
  size_t count = 0;
  while (true) {
    auto value = reader->NextValue();
    if (!value.ok()) break;
    if (walk != nullptr) {
      sum += walk(*value);
    } else {
      benchmark::DoNotOptimize(&*value);
    }
    ++count;
  }
  Require(count == data.entries, "ours: short read");
  return sum;
}

int64_t OursReadReuse(OursWalk walk) {
  const Fixture& data = Data();
  auto reader = bridge::DataFileReader::FromBytes(data.bytes);
  Require(reader.ok(), "ours reuse: reader construction");
  bridge::AvroValue value = bridge::AvroValue::CreateNull();
  int64_t sum = 0;
  size_t count = 0;
  while (true) {
    auto decoded = reader->NextValueInto(&value);
    Require(decoded.ok(), "ours reuse: decode");
    if (!*decoded) break;
    if (walk != nullptr) {
      sum += walk(value);
    } else {
      benchmark::DoNotOptimize(&value);
    }
    ++count;
  }
  Require(count == data.entries, "ours reuse: short read");
  return sum;
}

int64_t CppRead(CppWalk walk, bool project) {
  const Fixture& data = Data();
  auto in = avro::memoryInputStream(
      reinterpret_cast<const uint8_t*>(data.bytes.data()), data.bytes.size());
  std::unique_ptr<avro::DataFileReader<avro::GenericDatum>> reader =
      project ? std::make_unique<avro::DataFileReader<avro::GenericDatum>>(
                    std::move(in), data.cpp_projection)
              : std::make_unique<avro::DataFileReader<avro::GenericDatum>>(
                    std::move(in));
  avro::GenericDatum datum(reader->readerSchema());
  int64_t sum = 0;
  size_t count = 0;
  while (reader->read(datum)) {
    if (walk != nullptr) {
      sum += walk(datum);
    } else {
      benchmark::DoNotOptimize(&datum);
    }
    ++count;
  }
  Require(count == data.entries, "cpp: short read");
  return sum;
}

void RegisterOurs(const char* row, OursWalk ours, Mode mode) {
  const size_t entries = Data().entries;
  benchmark::RegisterBenchmark(
      std::string("ours/") + row,
      [ours, mode, entries](benchmark::State& state) {
        for (auto _ : state) benchmark::DoNotOptimize(OursRead(ours, mode));
        state.SetItemsProcessed(state.iterations() * entries);
      });
}

void RegisterOursReuse(const char* row, OursWalk ours) {
  const size_t entries = Data().entries;
  benchmark::RegisterBenchmark(
      std::string("ours_reuse/") + row,
      [ours, entries](benchmark::State& state) {
        for (auto _ : state) benchmark::DoNotOptimize(OursReadReuse(ours));
        state.SetItemsProcessed(state.iterations() * entries);
      });
}

void RegisterPair(const char* row, OursWalk ours, CppWalk cpp, bool project) {
  const size_t entries = Data().entries;
  RegisterOurs(row, ours, project ? Mode::kResolve : Mode::kFull);
  benchmark::RegisterBenchmark(
      std::string("avrocpp/") + row,
      [cpp, project, entries](benchmark::State& state) {
        for (auto _ : state) benchmark::DoNotOptimize(CppRead(cpp, project));
        state.SetItemsProcessed(state.iterations() * entries);
      });
}

}  // namespace

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;

  Fixture& data = Data();
  data.entries = EntryCount();
  const avro::ValidSchema schema =
      avro::compileJsonSchemaFromString(kManifestJson);
  data.cpp_projection = avro::compileJsonSchemaFromString(kProjectionJson);
  auto projection = bridge::AvroSchema::Parse(kProjectionJson);
  Require(projection.ok(), "ours: projection schema parse");
  data.projection = *std::move(projection);
  Require(bridge::AvroSchema::Parse(kManifestJson).ok(),
          "ours: manifest schema parse");

  std::vector<avro::GenericDatum> values;
  values.reserve(data.entries);
  for (size_t i = 0; i < data.entries; ++i) BuildEntry(schema, i, values);
  data.bytes = WriteCpp(schema, values);
  values.clear();
  values.shrink_to_fit();

  std::fprintf(stderr,
               "manifest: %zu entries, %zu columns, %zu bytes "
               "(%.0f bytes/entry; 150 MB is ~%.0f entries)\n",
               data.entries, static_cast<size_t>(Columns()), data.bytes.size(),
               static_cast<double>(data.bytes.size()) / data.entries,
               157286400.0 /
                   (static_cast<double>(data.bytes.size()) / data.entries));

  // Neither library may be doing less work than the other.
  Require(OursRead(OursPlannerWalk, Mode::kFull) == CppRead(CppPlannerWalk, false),
          "planner walk checksums disagree between the two libraries");
  Require(OursRead(OursFullWalk, Mode::kFull) == CppRead(CppFullWalk, false),
          "full walk checksums disagree between the two libraries");
  Require(OursReadReuse(OursPlannerWalk) == CppRead(CppPlannerWalk, false),
          "reusable planner walk disagrees with avrocpp");
  Require(OursReadReuse(OursFullWalk) == CppRead(CppFullWalk, false),
          "reusable full walk disagrees with avrocpp");
  Require(
      OursRead(OursProjectionWalk, Mode::kResolve) == CppRead(CppProjectionWalk, true),
      "projection walk checksums disagree between the two libraries");
  // The projection must also agree with the unprojected planner read on the
  // fields they share, or "projection" would just mean "reads less".
  Require(OursRead(OursProjectionWalk, Mode::kResolve) ==
              OursRead(OursProjectionWalk, Mode::kFull),
          "projected read disagrees with full-schema read on shared fields");

  // The new byte-level projection must agree with every other way of
  // reading the same fields, or the row below is measuring a wrong answer.
  Require(OursRead(OursProjectionWalk, Mode::kProject) ==
              CppRead(CppProjectionWalk, true),
          "native projection disagrees with avrocpp's projected read");
  Require(OursRead(OursProjectionWalk, Mode::kProject) ==
              OursRead(OursProjectionWalk, Mode::kFull),
          "native projection disagrees with our own full-schema read");

  RegisterPair("decode_only", nullptr, nullptr, false);
  RegisterPair("decode_planner", OursPlannerWalk, CppPlannerWalk, false);
  RegisterPair("decode_full", OursFullWalk, CppFullWalk, false);
  RegisterOursReuse("decode_only", nullptr);
  RegisterOursReuse("decode_planner", OursPlannerWalk);
  RegisterOursReuse("decode_full", OursFullWalk);
  RegisterPair("project_decode", OursProjectionWalk, CppProjectionWalk, true);
  // The point of stage 1: same fields as project_decode, but the unwanted
  // bytes are stepped over instead of materialized and discarded.
  RegisterOurs("project_native", OursProjectionWalk, Mode::kProject);

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
