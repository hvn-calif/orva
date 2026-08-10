// Throughput comparison: this repo's Rust-backed Avro binding vs Apache
// avro-cpp 1.11, doing identical work (same schemas, values, codecs,
// in-memory IO, single thread). See doc/specs/AvroBenchmark.md.
//
// Before any timing, a cross-read validation circle runs per dataset and
// codec: our writer's file is read by avrocpp, re-written by avrocpp, read
// back by us and compared value-by-value with the originals. A mismatch
// aborts the benchmark, so the numbers below can only ever compare two
// implementations doing the same thing.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "benchmark/benchmark.h"

#include "avro_bridge.h"
#include "avro_compare.h"

#include <avro/Compiler.hh>
#include <avro/DataFile.hh>
#include <avro/Decoder.hh>
#include <avro/Encoder.hh>
#include <avro/Generic.hh>
#include <avro/GenericDatum.hh>
#include <avro/Stream.hh>
#include <avro/ValidSchema.hh>

namespace bridge = security::avro;
namespace avro_compare = security::avro_compare;

namespace {

void Require(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FATAL: %s\n", what);
    std::exit(1);
  }
}

// ---------------------------------------------------------------------------
// Datasets: the same logical values materialized once per library.
// ---------------------------------------------------------------------------

struct Dataset {
  std::string name;
  size_t count;
  bridge::AvroSchema ours_schema;
  avro::ValidSchema cpp_schema;
  std::vector<bridge::AvroValue> ours_values;
  std::vector<avro::GenericDatum> cpp_values;
};

bridge::AvroSchema ParseOursSchema(const std::string& json) {
  auto schema = bridge::AvroSchema::Parse(json);
  Require(schema.ok(), "binding failed to parse a benchmark schema");
  return *std::move(schema);
}

// flat: {long id, string name}, 12-byte names; framing-bound.
Dataset BuildFlatDataset(size_t count) {
  const std::string json = R"({"type": "record", "name": "Flat", "fields": [
      {"name": "id", "type": "long"},
      {"name": "name", "type": "string"}]})";
  Dataset dataset{"flat", count, ParseOursSchema(json),
                  avro::compileJsonSchemaFromString(json), {}, {}};
  char name[16];
  for (size_t i = 0; i < count; ++i) {
    std::snprintf(name, sizeof(name), "sensor-%05zu", i % 100000);
    bridge::AvroValue record = bridge::AvroValue::CreateRecord();
    Require(record.RecordPut("id", bridge::AvroValue::CreateLong(
                                       static_cast<int64_t>(i)))
                .ok(),
            "flat: RecordPut id");
    auto str = bridge::AvroValue::CreateString(name);
    Require(str.ok() && record.RecordPut("name", *str).ok(),
            "flat: RecordPut name");
    dataset.ours_values.push_back(std::move(record));

    avro::GenericDatum datum(dataset.cpp_schema);
    auto& rec = datum.value<avro::GenericRecord>();
    rec.fieldAt(0) = avro::GenericDatum(static_cast<int64_t>(i));
    rec.fieldAt(1) = avro::GenericDatum(std::string(name));
    dataset.cpp_values.push_back(std::move(datum));
  }
  return dataset;
}

// nested: array of 8 sub-records plus a 4-key map; allocation-bound.
Dataset BuildNestedDataset(size_t count) {
  const std::string json = R"({"type": "record", "name": "Nested", "fields": [
      {"name": "items", "type": {"type": "array", "items":
          {"type": "record", "name": "Item", "fields": [
              {"name": "key", "type": "string"},
              {"name": "count", "type": "long"}]}}},
      {"name": "tags", "type": {"type": "map", "values": "long"}}]})";
  Dataset dataset{"nested", count, ParseOursSchema(json),
                  avro::compileJsonSchemaFromString(json), {}, {}};
  for (size_t i = 0; i < count; ++i) {
    bridge::AvroValue items = bridge::AvroValue::CreateArray();
    for (int j = 0; j < 8; ++j) {
      bridge::AvroValue item = bridge::AvroValue::CreateRecord();
      auto key = bridge::AvroValue::CreateString(
          "item-" + std::to_string(i) + "-" + std::to_string(j));
      Require(key.ok() && item.RecordPut("key", *key).ok(),
              "nested: item key");
      Require(item.RecordPut("count", bridge::AvroValue::CreateLong(
                                          static_cast<int64_t>(i * 8 + j)))
                  .ok(),
              "nested: item count");
      Require(items.ArrayPush(item).ok(), "nested: ArrayPush");
    }
    bridge::AvroValue tags = bridge::AvroValue::CreateMap();
    for (int j = 0; j < 4; ++j) {
      Require(tags.MapPut("tag-" + std::to_string(j),
                          bridge::AvroValue::CreateLong(
                              static_cast<int64_t>(i + j)))
                  .ok(),
              "nested: MapPut");
    }
    bridge::AvroValue record = bridge::AvroValue::CreateRecord();
    Require(record.RecordPut("items", items).ok(), "nested: put items");
    Require(record.RecordPut("tags", tags).ok(), "nested: put tags");
    dataset.ours_values.push_back(std::move(record));

    avro::GenericDatum datum(dataset.cpp_schema);
    auto& rec = datum.value<avro::GenericRecord>();
    auto& arr = rec.fieldAt(0).value<avro::GenericArray>();
    const avro::NodePtr item_node = arr.schema()->leafAt(0);
    for (int j = 0; j < 8; ++j) {
      avro::GenericDatum item(item_node);
      auto& item_rec = item.value<avro::GenericRecord>();
      item_rec.fieldAt(0) = avro::GenericDatum(
          "item-" + std::to_string(i) + "-" + std::to_string(j));
      item_rec.fieldAt(1) =
          avro::GenericDatum(static_cast<int64_t>(i * 8 + j));
      arr.value().push_back(std::move(item));
    }
    auto& map = rec.fieldAt(1).value<avro::GenericMap>();
    for (int j = 0; j < 4; ++j) {
      map.value().emplace_back(
          "tag-" + std::to_string(j),
          avro::GenericDatum(static_cast<int64_t>(i + j)));
    }
    dataset.cpp_values.push_back(std::move(datum));
  }
  return dataset;
}

// strings: one 1 KiB string field; memcpy-bound. Payload bytes vary per
// record so the compressing codecs do real work.
Dataset BuildStringsDataset(size_t count) {
  const std::string json = R"({"type": "record", "name": "Blob", "fields": [
      {"name": "payload", "type": "string"}]})";
  Dataset dataset{"strings", count, ParseOursSchema(json),
                  avro::compileJsonSchemaFromString(json), {}, {}};
  for (size_t i = 0; i < count; ++i) {
    std::string payload(1024, '\0');
    uint32_t rng = static_cast<uint32_t>(i) * 2654435761u + 12345u;
    for (char& c : payload) {
      rng = rng * 1664525u + 1013904223u;
      c = static_cast<char>('a' + (rng >> 24) % 26);
    }
    auto str = bridge::AvroValue::CreateString(payload);
    Require(str.ok(), "strings: CreateString");
    bridge::AvroValue record = bridge::AvroValue::CreateRecord();
    Require(record.RecordPut("payload", *str).ok(), "strings: RecordPut");
    dataset.ours_values.push_back(std::move(record));

    avro::GenericDatum datum(dataset.cpp_schema);
    datum.value<avro::GenericRecord>().fieldAt(0) =
        avro::GenericDatum(payload);
    dataset.cpp_values.push_back(std::move(datum));
  }
  return dataset;
}

// ---------------------------------------------------------------------------
// Per-library operations. Free functions, symmetrical per side.
// ---------------------------------------------------------------------------

struct CodecPair {
  const char* name;
  bridge::Codec ours;
  avro::Codec cpp;
};

constexpr CodecPair kCodecs[] = {
    {"null", bridge::Codec::kNull, avro::NULL_CODEC},
    {"deflate", bridge::Codec::kDeflate, avro::DEFLATE_CODEC},
    {"snappy", bridge::Codec::kSnappy, avro::SNAPPY_CODEC},
};

std::string OursWriteContainer(const Dataset& dataset, bridge::Codec codec) {
  auto writer = bridge::DataFileWriter::Create(dataset.ours_schema, codec);
  Require(writer.ok(), "binding: DataFileWriter::Create");
  for (const bridge::AvroValue& value : dataset.ours_values) {
    Require(writer->Append(value).ok(), "binding: Append");
  }
  auto bytes = writer->Finish();
  Require(bytes.ok(), "binding: Finish");
  return *std::move(bytes);
}

size_t OursReadContainer(const std::string& bytes) {
  auto reader = bridge::DataFileReader::FromBytes(bytes);
  Require(reader.ok(), "binding: FromBytes");
  size_t count = 0;
  while (true) {
    auto ready = reader->NextReady();
    Require(ready.ok(), "binding: NextReady");
    if (!*ready) break;
    auto value = reader->NextValue();
    Require(value.ok(), "binding: NextValue");
    benchmark::DoNotOptimize(&*value);
    ++count;
  }
  return count;
}

// Yields a buffer in fixed-size chunks, as a transport would. The timed
// benchmarks use 64 KiB; validation uses an odd size to hit arbitrary
// split points.
class ChunkedStream final : public bridge::ZeroCopyInputStream {
 public:
  explicit ChunkedStream(const std::string& data,
                         size_t chunk_size = 64 * 1024)
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

size_t OursStreamReadContainer(const std::string& bytes) {
  ChunkedStream stream(bytes);
  auto reader = bridge::DataFileStreamReader::Create(&stream);
  Require(reader.ok(), "binding: DataFileStreamReader::Create");
  size_t count = 0;
  while (true) {
    auto ready = reader->HasNext();
    Require(ready.ok(), "binding: HasNext");
    if (!*ready) break;
    auto value = reader->NextValue();
    Require(value.ok(), "binding: stream NextValue");
    benchmark::DoNotOptimize(&*value);
    ++count;
  }
  return count;
}

std::string CppWriteContainer(const Dataset& dataset, avro::Codec codec) {
  std::unique_ptr<avro::OutputStream> out = avro::memoryOutputStream();
  avro::OutputStream* raw = out.get();
  avro::DataFileWriter<avro::GenericDatum> writer(
      std::move(out), dataset.cpp_schema, 16 * 1024, codec);
  for (const avro::GenericDatum& value : dataset.cpp_values) {
    writer.write(value);
  }
  // flush() completes the file (writes the pending block and sync marker);
  // snapshot must happen before close(), which destroys the owned stream.
  writer.flush();
  auto snapshot = avro::snapshot(*raw);
  writer.close();
  return std::string(snapshot->begin(), snapshot->end());
}

size_t CppReadContainer(const std::string& bytes) {
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

std::vector<std::string> OursEncodeDatums(const Dataset& dataset) {
  std::vector<std::string> encoded;
  encoded.reserve(dataset.ours_values.size());
  for (const bridge::AvroValue& value : dataset.ours_values) {
    auto bytes = bridge::EncodeDatum(dataset.ours_schema, value);
    Require(bytes.ok(), "binding: EncodeDatum");
    encoded.push_back(*std::move(bytes));
  }
  return encoded;
}

size_t OursDecodeDatums(const Dataset& dataset,
                        const std::vector<std::string>& encoded) {
  size_t count = 0;
  for (const std::string& bytes : encoded) {
    auto value = bridge::DecodeDatum(dataset.ours_schema, bytes);
    Require(value.ok(), "binding: DecodeDatum");
    benchmark::DoNotOptimize(&*value);
    ++count;
  }
  return count;
}

// The two rows below hold one AvroDatumReader across the whole loop, so the
// writer schema's named types resolve once rather than per datum. That is the
// same shape as the avrocpp row, which builds one GenericReader up front.
size_t OursReaderDecodeDatums(const bridge::AvroDatumReader& reader,
                              const std::vector<std::string>& encoded) {
  size_t count = 0;
  for (const std::string& bytes : encoded) {
    auto value = reader.Decode(bytes);
    Require(value.ok(), "binding: AvroDatumReader::Decode");
    benchmark::DoNotOptimize(&*value);
    ++count;
  }
  return count;
}

// Also reuses one caller-owned value, exactly as the avrocpp row reuses one
// GenericDatum. This is the like-for-like comparison.
size_t OursReaderDecodeDatumsInto(const bridge::AvroDatumReader& reader,
                                  const std::vector<std::string>& encoded,
                                  bridge::AvroValue* value) {
  size_t count = 0;
  for (const std::string& bytes : encoded) {
    Require(reader.DecodeInto(bytes, value).ok(),
            "binding: AvroDatumReader::DecodeInto");
    benchmark::DoNotOptimize(value);
    ++count;
  }
  return count;
}

// avro-cpp's encoder is a stream: all datums go into one buffer. Our
// binding's EncodeDatum/DecodeDatum is one buffer per datum. Each side is
// benchmarked in its natural mode; the API-shape difference is part of
// what is being measured (see benchmarks/README.md).
std::string CppEncodeDatums(const Dataset& dataset) {
  std::unique_ptr<avro::OutputStream> out = avro::memoryOutputStream();
  avro::EncoderPtr encoder = avro::binaryEncoder();
  encoder->init(*out);
  avro::GenericWriter writer(dataset.cpp_schema, encoder);
  for (const avro::GenericDatum& value : dataset.cpp_values) {
    writer.write(value);
  }
  encoder->flush();
  auto snapshot = avro::snapshot(*out);
  return std::string(snapshot->begin(), snapshot->end());
}

size_t CppDecodeDatums(const Dataset& dataset, const std::string& encoded,
                       size_t count) {
  auto in = avro::memoryInputStream(
      reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size());
  avro::DecoderPtr decoder = avro::binaryDecoder();
  decoder->init(*in);
  avro::GenericReader reader(dataset.cpp_schema, decoder);
  avro::GenericDatum datum(dataset.cpp_schema);
  for (size_t i = 0; i < count; ++i) {
    reader.read(datum);
    benchmark::DoNotOptimize(&datum);
  }
  return count;
}

// ---------------------------------------------------------------------------
// Cross-read validation: ours-write -> avrocpp-read -> avrocpp-write ->
// ours-read -> compare with the originals. Runs on a small prefix of each
// dataset for every codec, before anything is timed.
// ---------------------------------------------------------------------------

void ValidateCrossRead(const Dataset& dataset) {
  // Above 1024 values so our writer's batching produces multiple container
  // blocks: the circle then exercises the same block-boundary code paths
  // the timed benchmarks run through, not just a single-block file.
  const size_t sample_count = std::min<size_t>(2000, dataset.count);
  const absl::Span<const bridge::AvroValue> sample(
      dataset.ours_values.data(), sample_count);
  for (const CodecPair& codec : kCodecs) {
    avro_compare::CompareResult result = avro_compare::CrossReadCircle(
        dataset.ours_schema, dataset.cpp_schema, sample, codec.ours,
        codec.cpp);
    Require(result.ok(),
            (dataset.name + "/" + codec.name + ": validate: " +
             result.diverged_at)
                .c_str());
  }
  std::fprintf(stderr, "validated cross-read circle: %s\n",
               dataset.name.c_str());
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

std::string BenchName(const char* lib, const char* op, const Dataset& dataset,
                      const char* codec) {
  std::string name = std::string(lib) + "/" + op + "/" + dataset.name;
  if (codec != nullptr) name += std::string("/") + codec;
  return name;
}

void RegisterContainerBenchmarks(const Dataset& dataset) {
  for (const CodecPair& codec : kCodecs) {
    // The read benchmarks for both libraries consume the same bytes (the
    // avrocpp-written file) so the decode work is identical.
    auto common_file =
        std::make_shared<std::string>(CppWriteContainer(dataset, codec.cpp));
    const size_t file_size = common_file->size();
    const size_t count = dataset.count;

    benchmark::RegisterBenchmark(
        BenchName("ours", "container_write", dataset, codec.name),
        [&dataset, codec](benchmark::State& state) {
          size_t bytes = 0;
          for (auto _ : state) {
            std::string file = OursWriteContainer(dataset, codec.ours);
            // iterations * bytes below assumes deterministic output size.
            Require(bytes == 0 || bytes == file.size(),
                    "benchmark: nondeterministic write output size");
            bytes = file.size();
            benchmark::DoNotOptimize(file.data());
          }
          state.SetItemsProcessed(state.iterations() * dataset.count);
          state.SetBytesProcessed(state.iterations() * bytes);
        });
    benchmark::RegisterBenchmark(
        BenchName("avrocpp", "container_write", dataset, codec.name),
        [&dataset, codec](benchmark::State& state) {
          size_t bytes = 0;
          for (auto _ : state) {
            std::string file = CppWriteContainer(dataset, codec.cpp);
            Require(bytes == 0 || bytes == file.size(),
                    "benchmark: nondeterministic write output size");
            bytes = file.size();
            benchmark::DoNotOptimize(file.data());
          }
          state.SetItemsProcessed(state.iterations() * dataset.count);
          state.SetBytesProcessed(state.iterations() * bytes);
        });
    benchmark::RegisterBenchmark(
        BenchName("ours", "container_read", dataset, codec.name),
        [common_file, count, file_size](benchmark::State& state) {
          for (auto _ : state) {
            Require(OursReadContainer(*common_file) == count,
                    "benchmark: short read");
          }
          state.SetItemsProcessed(state.iterations() * count);
          state.SetBytesProcessed(state.iterations() * file_size);
        });
    benchmark::RegisterBenchmark(
        BenchName("avrocpp", "container_read", dataset, codec.name),
        [common_file, count, file_size](benchmark::State& state) {
          for (auto _ : state) {
            Require(CppReadContainer(*common_file) == count,
                    "benchmark: short read");
          }
          state.SetItemsProcessed(state.iterations() * count);
          state.SetBytesProcessed(state.iterations() * file_size);
        });
    benchmark::RegisterBenchmark(
        BenchName("ours", "stream_read_64k", dataset, codec.name),
        [common_file, count, file_size](benchmark::State& state) {
          for (auto _ : state) {
            Require(OursStreamReadContainer(*common_file) == count,
                    "benchmark: short stream read");
          }
          state.SetItemsProcessed(state.iterations() * count);
          state.SetBytesProcessed(state.iterations() * file_size);
        });
  }
}

void RegisterDatumBenchmarks(const Dataset& dataset) {
  auto ours_encoded = std::make_shared<std::vector<std::string>>(
      OursEncodeDatums(dataset));
  auto cpp_encoded =
      std::make_shared<std::string>(CppEncodeDatums(dataset));
  size_t ours_bytes = 0;
  for (const std::string& datum : *ours_encoded) ours_bytes += datum.size();
  const size_t count = dataset.count;

  benchmark::RegisterBenchmark(
      BenchName("ours", "datum_encode", dataset, nullptr),
      [&dataset, ours_bytes](benchmark::State& state) {
        for (auto _ : state) {
          std::vector<std::string> encoded = OursEncodeDatums(dataset);
          benchmark::DoNotOptimize(encoded.data());
        }
        state.SetItemsProcessed(state.iterations() * dataset.count);
        state.SetBytesProcessed(state.iterations() * ours_bytes);
      });
  benchmark::RegisterBenchmark(
      BenchName("avrocpp", "datum_encode", dataset, nullptr),
      [&dataset](benchmark::State& state) {
        size_t bytes = 0;
        for (auto _ : state) {
          std::string encoded = CppEncodeDatums(dataset);
          Require(bytes == 0 || bytes == encoded.size(),
                  "benchmark: nondeterministic encode output size");
          bytes = encoded.size();
          benchmark::DoNotOptimize(encoded.data());
        }
        state.SetItemsProcessed(state.iterations() * dataset.count);
        state.SetBytesProcessed(state.iterations() * bytes);
      });
  benchmark::RegisterBenchmark(
      BenchName("ours", "datum_decode", dataset, nullptr),
      [&dataset, ours_encoded, ours_bytes, count](benchmark::State& state) {
        for (auto _ : state) {
          Require(OursDecodeDatums(dataset, *ours_encoded) == count,
                  "benchmark: short datum decode");
        }
        state.SetItemsProcessed(state.iterations() * count);
        state.SetBytesProcessed(state.iterations() * ours_bytes);
      });
  benchmark::RegisterBenchmark(
      BenchName("ours_reader", "datum_decode", dataset, nullptr),
      [&dataset, ours_encoded, ours_bytes, count](benchmark::State& state) {
        auto reader = bridge::AvroDatumReader::Create(dataset.ours_schema);
        Require(reader.ok(), "binding: AvroDatumReader::Create");
        for (auto _ : state) {
          Require(OursReaderDecodeDatums(*reader, *ours_encoded) == count,
                  "benchmark: short datum decode");
        }
        state.SetItemsProcessed(state.iterations() * count);
        state.SetBytesProcessed(state.iterations() * ours_bytes);
      });
  benchmark::RegisterBenchmark(
      BenchName("ours_reader_into", "datum_decode", dataset, nullptr),
      [&dataset, ours_encoded, ours_bytes, count](benchmark::State& state) {
        auto reader = bridge::AvroDatumReader::Create(dataset.ours_schema);
        Require(reader.ok(), "binding: AvroDatumReader::Create");
        bridge::AvroValue value = bridge::AvroValue::CreateNull();
        for (auto _ : state) {
          Require(
              OursReaderDecodeDatumsInto(*reader, *ours_encoded, &value) == count,
              "benchmark: short datum decode");
        }
        state.SetItemsProcessed(state.iterations() * count);
        state.SetBytesProcessed(state.iterations() * ours_bytes);
      });
  benchmark::RegisterBenchmark(
      BenchName("avrocpp", "datum_decode", dataset, nullptr),
      [&dataset, cpp_encoded, count](benchmark::State& state) {
        for (auto _ : state) {
          Require(CppDecodeDatums(dataset, *cpp_encoded, count) == count,
                  "benchmark: short datum decode");
        }
        state.SetItemsProcessed(state.iterations() * count);
        state.SetBytesProcessed(state.iterations() * cpp_encoded->size());
      });
}

}  // namespace

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;

  std::vector<Dataset> datasets;
  datasets.push_back(BuildFlatDataset(100000));
  datasets.push_back(BuildNestedDataset(10000));
  datasets.push_back(BuildStringsDataset(10000));

  for (const Dataset& dataset : datasets) {
    ValidateCrossRead(dataset);
  }
  for (const Dataset& dataset : datasets) {
    RegisterContainerBenchmarks(dataset);
    RegisterDatumBenchmarks(dataset);
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
