// Scaling sweeps: how the ours-vs-avrocpp read gap varies with payload size
// and with field count. The point is to separate a fixed per-record cost
// from costs that scale with the data, and to find where (if anywhere) the
// binding reaches parity.
//
// Sweep A (payload): record {string payload, int age}, payload 0..4096
// bytes. A fixed per-record overhead shows up as a ratio that decays toward
// 1 as the payload grows.
//
// Sweep B (fields): record of N int fields, N = 1..16. Our decoded
// Value::Record heap-allocates one String per field *name* per record;
// avro-cpp keeps names in the shared schema node. A per-field-name cost
// shows up as a steeper ns/record slope on our side.
//
// Both sweeps read a container file written by avro-cpp (default 16 KiB
// blocking, null codec) so both sides consume identical bytes.

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

size_t OursRead(const std::string& bytes) {
  auto reader = bridge::DataFileReader::FromBytes(bytes);
  Require(reader.ok(), "ours: FromBytes");
  size_t count = 0;
  while (true) {
    auto value = reader->NextValue();
    if (!value.ok()) break;
    benchmark::DoNotOptimize(&*value);
    ++count;
  }
  return count;
}

size_t CppRead(const std::string& bytes) {
  auto in = avro::memoryInputStream(
      reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  avro::DataFileReader<avro::GenericDatum> reader(std::move(in));
  avro::GenericDatum datum(reader.dataSchema());
  size_t count = 0;
  while (reader.read(datum)) {
    benchmark::DoNotOptimize(&datum);
    ++count;
  }
  return count;
}

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

// Files must outlive registration; benchmarks hold pointers into this.
std::vector<std::string>& FileStore() {
  static std::vector<std::string> store;
  return store;
}

void RegisterPair(const std::string& label, const std::string& file) {
  FileStore().push_back(file);
  const std::string* held = &FileStore().back();
  Require(OursRead(*held) == kCount, "ours: short read");
  Require(CppRead(*held) == kCount, "cpp: short read");
  benchmark::RegisterBenchmark("ours/" + label,
                               [held](benchmark::State& state) {
                                 for (auto _ : state)
                                   Require(OursRead(*held) == kCount, "short");
                                 state.SetItemsProcessed(state.iterations() *
                                                         kCount);
                               });
  benchmark::RegisterBenchmark("avrocpp/" + label,
                               [held](benchmark::State& state) {
                                 for (auto _ : state)
                                   Require(CppRead(*held) == kCount, "short");
                                 state.SetItemsProcessed(state.iterations() *
                                                         kCount);
                               });
}

void SweepPayload(size_t payload_len) {
  const std::string json =
      R"({"type":"record","name":"P)" + std::to_string(payload_len) +
      R"(","fields":[{"name":"payload","type":"string"},)"
      R"({"name":"age","type":"int"}]})";
  const avro::ValidSchema schema = avro::compileJsonSchemaFromString(json);
  std::vector<avro::GenericDatum> values;
  // Payload bytes vary per record so nothing degenerates into one shared
  // allocation or a trivially predictable branch.
  for (size_t i = 0; i < kCount; ++i) {
    std::string payload(payload_len, '\0');
    uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 12345u;
    for (char& c : payload) {
      rng = rng * 1664525u + 1013904223u;
      c = static_cast<char>('a' + (rng >> 24) % 26);
    }
    avro::GenericDatum datum(schema);
    auto& rec = datum.value<avro::GenericRecord>();
    rec.fieldAt(0) = avro::GenericDatum(payload);
    rec.fieldAt(1) = avro::GenericDatum(static_cast<int32_t>(25));
    values.push_back(std::move(datum));
  }
  char label[64];
  std::snprintf(label, sizeof(label), "payload/%04zuB", payload_len);
  RegisterPair(label, WriteCpp(schema, values));
}

void SweepFields(size_t field_count) {
  std::string json = R"({"type":"record","name":"F)" +
                     std::to_string(field_count) + R"(","fields":[)";
  for (size_t f = 0; f < field_count; ++f) {
    if (f > 0) json += ",";
    // Field names are a realistic length; they are what our decoder
    // heap-allocates once per record per field.
    json += R"({"name":"field_number_)" + std::to_string(f) +
            R"(","type":"int"})";
  }
  json += "]}";
  const avro::ValidSchema schema = avro::compileJsonSchemaFromString(json);
  std::vector<avro::GenericDatum> values;
  for (size_t i = 0; i < kCount; ++i) {
    avro::GenericDatum datum(schema);
    auto& rec = datum.value<avro::GenericRecord>();
    for (size_t f = 0; f < field_count; ++f) {
      rec.fieldAt(f) = avro::GenericDatum(static_cast<int32_t>(i + f));
    }
    values.push_back(std::move(datum));
  }
  char label[64];
  std::snprintf(label, sizeof(label), "fields/%02zu", field_count);
  RegisterPair(label, WriteCpp(schema, values));
}

}  // namespace

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;

  // Reserve so RegisterPair's pointers into the store stay valid.
  FileStore().reserve(32);
  for (size_t len : {size_t{0}, size_t{4}, size_t{16}, size_t{64},
                     size_t{256}, size_t{1024}, size_t{4096}}) {
    SweepPayload(len);
  }
  for (size_t n : {size_t{1}, size_t{2}, size_t{4}, size_t{8}, size_t{16}}) {
    SweepFields(n);
  }

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
