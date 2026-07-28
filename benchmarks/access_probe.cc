// What it costs to read a decoded value back out, field by field.
//
// Every other benchmark here decodes a value and drops it. Real callers read
// it, and our accessors return clones: get_record_field and get_array_item
// each copy the subtree they return (rust/value.rs), because the FFI
// boundary cannot hand back a reference. avro-cpp's GenericDatum accessors
// return references and copy nothing.
//
// Rows:
//   decode_only    decode each record, touch nothing (what the other
//                  benchmarks measure)
//   decode_walk    decode each record and read every leaf out of it
//   walk_only      decode_walk minus decode_only, computed by the reader
//
// Also measures a minimal FFI crossing (DataFileReader::AtEnd, which reads
// one bool out of the Rust struct) to bound what batching the value-pulling
// API could ever save.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
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

constexpr size_t kCount = 10000;

void Require(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FATAL: %s\n", what);
    std::exit(1);
  }
}

// A record with an 8-item array of sub-records and a 4-key map: enough
// structure that cloning a subtree to reach a leaf is visible.
const char* kNestedJson = R"({"type":"record","name":"Nested","fields":[
    {"name":"items","type":{"type":"array","items":
        {"type":"record","name":"Item","fields":[
            {"name":"key","type":"string"},
            {"name":"count","type":"long"}]}}},
    {"name":"tags","type":{"type":"map","values":"long"}}]})";

// A flat record, to separate per-leaf cost from per-subtree cost.
const char* kFlatJson = R"({"type":"record","name":"Flat","fields":[
    {"name":"id","type":"long"},{"name":"name","type":"string"}]})";

std::string WriteCpp(const avro::ValidSchema& schema,
                     const std::vector<avro::GenericDatum>& values) {
  std::unique_ptr<avro::OutputStream> out = avro::memoryOutputStream();
  avro::OutputStream* raw = out.get();
  avro::DataFileWriter<avro::GenericDatum> writer(std::move(out), schema,
                                                  16 * 1024,
                                                  avro::NULL_CODEC);
  for (const avro::GenericDatum& value : values) writer.write(value);
  writer.flush();
  auto snapshot = avro::snapshot(*raw);
  writer.close();
  return std::string(snapshot->begin(), snapshot->end());
}

// ---------------------------------------------------------------------------
// Walks. Each returns a checksum so nothing can be optimized away, and each
// side reads exactly the same leaves.
// ---------------------------------------------------------------------------

int64_t OursWalkNested(const bridge::AvroValue& record) {
  int64_t sum = 0;
  auto items = record.GetRecordField("items");
  Require(items.ok(), "ours: items");
  auto len = items->GetArrayLen();
  Require(len.ok(), "ours: array len");
  for (uint64_t i = 0; i < *len; ++i) {
    auto item = items->GetArrayItem(i);
    Require(item.ok(), "ours: item");
    auto key = item->GetRecordField("key");
    Require(key.ok(), "ours: key");
    auto key_str = key->GetString();
    Require(key_str.ok(), "ours: key string");
    sum += static_cast<int64_t>(key_str->size());
    auto count = item->GetRecordField("count");
    Require(count.ok(), "ours: count");
    sum += count->GetLong().value_or(0);
  }
  auto tags = record.GetRecordField("tags");
  Require(tags.ok(), "ours: tags");
  auto keys = tags->GetMapKeys();
  Require(keys.ok(), "ours: map keys");
  for (const std::string& key : *keys) {
    auto value = tags->GetMapValue(key);
    Require(value.ok(), "ours: map value");
    sum += value->GetLong().value_or(0);
  }
  return sum;
}

// The same leaves, reached by path. One path is built per record and reused
// across the array with SetLastIndex, so nothing on the way to a leaf is
// copied. (Building it per record rather than once per file keeps this
// honest: a caller with a fixed schema could hoist it further.)
int64_t OursWalkNestedByPath(const bridge::AvroValue& record) {
  int64_t sum = 0;
  bridge::AvroPath path;
  Require(path.PushField("items").ok(), "path: items");
  auto len = record.GetArrayLenAt(path);
  Require(len.ok(), "path: array len");
  path.PushIndex(0);
  for (uint64_t i = 0; i < *len; ++i) {
    Require(path.SetLastIndex(i).ok(), "path: set index");
    Require(path.PushField("key").ok(), "path: key");
    auto key = record.GetStringAt(path);
    Require(key.ok(), "path: key read");
    sum += static_cast<int64_t>(key->size());
    Require(path.Pop().ok(), "path: pop key");
    Require(path.PushField("count").ok(), "path: count");
    sum += record.GetLongAt(path).value_or(0);
    Require(path.Pop().ok(), "path: pop count");
  }

  bridge::AvroPath tags;
  Require(tags.PushField("tags").ok(), "path: tags");
  auto keys = record.GetMapKeysAt(tags);
  Require(keys.ok(), "path: map keys");
  for (const std::string& key : *keys) {
    Require(tags.PushKey(key).ok(), "path: push key");
    sum += record.GetLongAt(tags).value_or(0);
    Require(tags.Pop().ok(), "path: pop key");
  }
  return sum;
}

int64_t OursWalkFlatByPath(const bridge::AvroValue& record) {
  bridge::AvroPath path;
  Require(path.PushField("id").ok(), "path: id");
  const int64_t id = record.GetLongAt(path).value_or(0);
  Require(path.Pop().ok(), "path: pop id");
  Require(path.PushField("name").ok(), "path: name");
  auto name = record.GetStringAt(path);
  Require(name.ok(), "path: name read");
  return id + static_cast<int64_t>(name->size());
}

int64_t CppWalkNested(const avro::GenericDatum& datum) {
  int64_t sum = 0;
  const auto& rec = datum.value<avro::GenericRecord>();
  const auto& arr = rec.fieldAt(0).value<avro::GenericArray>();
  for (const avro::GenericDatum& item : arr.value()) {
    const auto& item_rec = item.value<avro::GenericRecord>();
    sum += static_cast<int64_t>(
        item_rec.fieldAt(0).value<std::string>().size());
    sum += item_rec.fieldAt(1).value<int64_t>();
  }
  const auto& map = rec.fieldAt(1).value<avro::GenericMap>();
  for (const auto& entry : map.value()) {
    sum += entry.second.value<int64_t>();
  }
  return sum;
}

int64_t OursWalkFlat(const bridge::AvroValue& record) {
  auto id = record.GetRecordField("id");
  Require(id.ok(), "ours: id");
  auto name = record.GetRecordField("name");
  Require(name.ok(), "ours: name");
  auto name_str = name->GetString();
  Require(name_str.ok(), "ours: name string");
  return id->GetLong().value_or(0) + static_cast<int64_t>(name_str->size());
}

int64_t CppWalkFlat(const avro::GenericDatum& datum) {
  const auto& rec = datum.value<avro::GenericRecord>();
  return rec.fieldAt(0).value<int64_t>() +
         static_cast<int64_t>(rec.fieldAt(1).value<std::string>().size());
}

using OursWalk = int64_t (*)(const bridge::AvroValue&);
using CppWalk = int64_t (*)(const avro::GenericDatum&);

int64_t OursRead(const std::string& bytes, OursWalk walk) {
  auto reader = bridge::DataFileReader::FromBytes(bytes);
  Require(reader.ok(), "ours: FromBytes");
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
  Require(count == kCount, "ours: short read");
  return sum;
}

int64_t CppRead(const std::string& bytes, CppWalk walk) {
  auto in = avro::memoryInputStream(
      reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  avro::DataFileReader<avro::GenericDatum> reader(std::move(in));
  avro::GenericDatum datum(reader.dataSchema());
  int64_t sum = 0;
  size_t count = 0;
  while (reader.read(datum)) {
    if (walk != nullptr) {
      sum += walk(datum);
    } else {
      benchmark::DoNotOptimize(&datum);
    }
    ++count;
  }
  Require(count == kCount, "cpp: short read");
  return sum;
}

std::vector<std::string>& FileStore() {
  static std::vector<std::string> store;
  return store;
}

void RegisterDataset(const char* label, const char* json,
                     void (*build)(const avro::ValidSchema&, size_t,
                                   std::vector<avro::GenericDatum>&),
                     OursWalk ours_walk, OursWalk ours_path_walk,
                     CppWalk cpp_walk) {
  const avro::ValidSchema schema = avro::compileJsonSchemaFromString(json);
  std::vector<avro::GenericDatum> values;
  for (size_t i = 0; i < kCount; ++i) build(schema, i, values);
  FileStore().push_back(WriteCpp(schema, values));
  const std::string* file = &FileStore().back();

  // Both sides must agree on what the walk computes, or they are not doing
  // the same work.
  Require(OursRead(*file, ours_walk) == CppRead(*file, cpp_walk),
          "walk checksums disagree between the two libraries");
  Require(OursRead(*file, ours_path_walk) == CppRead(*file, cpp_walk),
          "path walk disagrees with the cloning walk");

  benchmark::RegisterBenchmark(
      std::string("ours/decode_only/") + label,
      [file](benchmark::State& state) {
        for (auto _ : state) benchmark::DoNotOptimize(OursRead(*file, nullptr));
        state.SetItemsProcessed(state.iterations() * kCount);
      });
  benchmark::RegisterBenchmark(
      std::string("ours/decode_walk/") + label,
      [file, ours_walk](benchmark::State& state) {
        for (auto _ : state)
          benchmark::DoNotOptimize(OursRead(*file, ours_walk));
        state.SetItemsProcessed(state.iterations() * kCount);
      });
  benchmark::RegisterBenchmark(
      std::string("ours/decode_walk_path/") + label,
      [file, ours_path_walk](benchmark::State& state) {
        for (auto _ : state)
          benchmark::DoNotOptimize(OursRead(*file, ours_path_walk));
        state.SetItemsProcessed(state.iterations() * kCount);
      });
  benchmark::RegisterBenchmark(
      std::string("avrocpp/decode_only/") + label,
      [file](benchmark::State& state) {
        for (auto _ : state) benchmark::DoNotOptimize(CppRead(*file, nullptr));
        state.SetItemsProcessed(state.iterations() * kCount);
      });
  benchmark::RegisterBenchmark(
      std::string("avrocpp/decode_walk/") + label,
      [file, cpp_walk](benchmark::State& state) {
        for (auto _ : state)
          benchmark::DoNotOptimize(CppRead(*file, cpp_walk));
        state.SetItemsProcessed(state.iterations() * kCount);
      });
}

void BuildNested(const avro::ValidSchema& schema, size_t i,
                 std::vector<avro::GenericDatum>& out) {
  avro::GenericDatum datum(schema);
  auto& rec = datum.value<avro::GenericRecord>();
  auto& arr = rec.fieldAt(0).value<avro::GenericArray>();
  const avro::NodePtr item_node = arr.schema()->leafAt(0);
  for (int j = 0; j < 8; ++j) {
    avro::GenericDatum item(item_node);
    auto& item_rec = item.value<avro::GenericRecord>();
    item_rec.fieldAt(0) = avro::GenericDatum(
        "item-" + std::to_string(i) + "-" + std::to_string(j));
    item_rec.fieldAt(1) = avro::GenericDatum(static_cast<int64_t>(i * 8 + j));
    arr.value().push_back(std::move(item));
  }
  auto& map = rec.fieldAt(1).value<avro::GenericMap>();
  for (int j = 0; j < 4; ++j) {
    map.value().emplace_back("tag-" + std::to_string(j),
                             avro::GenericDatum(static_cast<int64_t>(i + j)));
  }
  out.push_back(std::move(datum));
}

void BuildFlat(const avro::ValidSchema& schema, size_t i,
               std::vector<avro::GenericDatum>& out) {
  avro::GenericDatum datum(schema);
  auto& rec = datum.value<avro::GenericRecord>();
  rec.fieldAt(0) = avro::GenericDatum(static_cast<int64_t>(i));
  rec.fieldAt(1) = avro::GenericDatum("sensor-" + std::to_string(i));
  out.push_back(std::move(datum));
}

}  // namespace

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  FileStore().reserve(8);
  RegisterDataset("flat", kFlatJson, BuildFlat, OursWalkFlat,
                  OursWalkFlatByPath, CppWalkFlat);
  RegisterDataset("nested", kNestedJson, BuildNested, OursWalkNested,
                  OursWalkNestedByPath, CppWalkNested);

  // A minimal FFI crossing: AtEnd reads one bool out of the Rust struct and
  // allocates nothing. This is the floor for any per-call bridge cost, and
  // so bounds what a batched value-pulling API could save.
  static const std::string& probe_file = FileStore().front();
  benchmark::RegisterBenchmark("ffi/at_end_call", [](benchmark::State& state) {
    auto reader = bridge::DataFileReader::FromBytes(probe_file);
    Require(reader.ok(), "ffi: FromBytes");
    for (auto _ : state) {
      benchmark::DoNotOptimize(reader->AtEnd());
    }
  });

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
