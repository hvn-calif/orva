// What the copy inside DataFileWriter::Append costs, and what moving saves.
//
// avro_benchmark.cc builds its values once and appends the same objects on
// every iteration, so it cannot show this: a caller that keeps its values
// has to pay the copy. This probe builds each value inside the timed region,
// which is how a producer actually writes -- construct a record, hand it to
// the writer, never look at it again -- and appends it three ways:
//
//   ours/build_copy   build the value, Append(const AvroValue&)
//   ours/build_move   build the value, Append(AvroValue&&)
//   avrocpp/build     build a GenericDatum, write() it
//
// The build cost is common to all three rows, so the copy/move difference is
// the gap between the first two. avrocpp's row is here for scale, not as the
// thing being isolated.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
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

constexpr size_t kCount = 10000;

void Require(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FATAL: %s\n", what);
    std::exit(1);
  }
}

// flat: two scalar fields. The copy is a Vec plus two field-name Strings
// plus the payload String.
const char* kFlatJson = R"({"type":"record","name":"Flat","fields":[
    {"name":"id","type":"long"},{"name":"name","type":"string"}]})";

// nested: an 8-item array of sub-records and a 4-key map, so the copy walks
// a real tree rather than three allocations.
const char* kNestedJson = R"({"type":"record","name":"Nested","fields":[
    {"name":"items","type":{"type":"array","items":
        {"type":"record","name":"Item","fields":[
            {"name":"key","type":"string"},
            {"name":"count","type":"long"}]}}},
    {"name":"tags","type":{"type":"map","values":"long"}}]})";

bridge::AvroValue BuildFlat(size_t i) {
  bridge::AvroValue record = bridge::AvroValue::CreateRecord();
  Require(record.RecordPut("id", bridge::AvroValue::CreateLong(
                                     static_cast<int64_t>(i)))
              .ok(),
          "flat: id");
  auto name = bridge::AvroValue::CreateString("sensor-" + std::to_string(i));
  Require(name.ok() && record.RecordPut("name", *name).ok(), "flat: name");
  return record;
}

bridge::AvroValue BuildNested(size_t i) {
  bridge::AvroValue items = bridge::AvroValue::CreateArray();
  for (int j = 0; j < 8; ++j) {
    bridge::AvroValue item = bridge::AvroValue::CreateRecord();
    auto key = bridge::AvroValue::CreateString(
        "item-" + std::to_string(i) + "-" + std::to_string(j));
    Require(key.ok() && item.RecordPut("key", *key).ok(), "nested: key");
    Require(item.RecordPut("count", bridge::AvroValue::CreateLong(
                                        static_cast<int64_t>(i * 8 + j)))
                .ok(),
            "nested: count");
    Require(items.ArrayPush(item).ok(), "nested: push");
  }
  bridge::AvroValue tags = bridge::AvroValue::CreateMap();
  for (int j = 0; j < 4; ++j) {
    Require(tags.MapPut("tag-" + std::to_string(j),
                        bridge::AvroValue::CreateLong(
                            static_cast<int64_t>(i + j)))
                .ok(),
            "nested: tag");
  }
  bridge::AvroValue record = bridge::AvroValue::CreateRecord();
  Require(record.RecordPut("items", items).ok(), "nested: items");
  Require(record.RecordPut("tags", tags).ok(), "nested: tags");
  return record;
}

avro::GenericDatum BuildCppFlat(const avro::ValidSchema& schema, size_t i) {
  avro::GenericDatum datum(schema);
  auto& rec = datum.value<avro::GenericRecord>();
  rec.fieldAt(0) = avro::GenericDatum(static_cast<int64_t>(i));
  rec.fieldAt(1) = avro::GenericDatum("sensor-" + std::to_string(i));
  return datum;
}

avro::GenericDatum BuildCppNested(const avro::ValidSchema& schema, size_t i) {
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
  return datum;
}

using OursBuilder = bridge::AvroValue (*)(size_t);
using CppBuilder = avro::GenericDatum (*)(const avro::ValidSchema&, size_t);

void RegisterDataset(const char* label, const char* json, OursBuilder ours,
                     CppBuilder cpp) {
  auto schema = bridge::AvroSchema::Parse(json);
  Require(schema.ok(), "schema parse");
  static std::vector<bridge::AvroSchema> ours_schemas;
  static std::vector<avro::ValidSchema> cpp_schemas;
  ours_schemas.reserve(8);
  cpp_schemas.reserve(8);
  ours_schemas.push_back(*std::move(schema));
  cpp_schemas.push_back(avro::compileJsonSchemaFromString(json));
  const bridge::AvroSchema* ours_schema = &ours_schemas.back();
  const avro::ValidSchema* cpp_schema = &cpp_schemas.back();

  benchmark::RegisterBenchmark(
      std::string("ours/build_copy/") + label,
      [ours_schema, ours](benchmark::State& state) {
        for (auto _ : state) {
          auto writer =
              bridge::DataFileWriter::Create(*ours_schema, bridge::Codec::kNull);
          Require(writer.ok(), "create");
          for (size_t i = 0; i < kCount; ++i) {
            bridge::AvroValue value = ours(i);
            Require(writer->Append(value).ok(), "append copy");
          }
          Require(writer->Finish().ok(), "finish");
        }
        state.SetItemsProcessed(state.iterations() * kCount);
      });
  benchmark::RegisterBenchmark(
      std::string("ours/build_move/") + label,
      [ours_schema, ours](benchmark::State& state) {
        for (auto _ : state) {
          auto writer =
              bridge::DataFileWriter::Create(*ours_schema, bridge::Codec::kNull);
          Require(writer.ok(), "create");
          for (size_t i = 0; i < kCount; ++i) {
            bridge::AvroValue value = ours(i);
            Require(writer->Append(std::move(value)).ok(), "append move");
          }
          Require(writer->Finish().ok(), "finish");
        }
        state.SetItemsProcessed(state.iterations() * kCount);
      });
  benchmark::RegisterBenchmark(
      std::string("avrocpp/build/") + label,
      [cpp_schema, cpp](benchmark::State& state) {
        for (auto _ : state) {
          std::unique_ptr<avro::OutputStream> out = avro::memoryOutputStream();
          avro::DataFileWriter<avro::GenericDatum> writer(
              std::move(out), *cpp_schema, 16 * 1024, avro::NULL_CODEC);
          for (size_t i = 0; i < kCount; ++i) {
            writer.write(cpp(*cpp_schema, i));
          }
          writer.flush();
          writer.close();
        }
        state.SetItemsProcessed(state.iterations() * kCount);
      });
}

}  // namespace

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;
  RegisterDataset("flat", kFlatJson, BuildFlat, BuildCppFlat);
  RegisterDataset("nested", kNestedJson, BuildNested, BuildCppNested);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
