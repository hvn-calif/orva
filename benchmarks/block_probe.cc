// Per-block vs per-value cost on a minimal record, and what a
// contiguous-struct traversal of the same data costs.
//
// Dataset is the reported one: UserRecord {string username, int age},
// 10000 records whose encoded form is exactly the 6 bytes "\x08test\x32"
// (username="test", age=25). The probe asserts that byte pattern, so the
// numbers describe that file and not an approximation of it.
//
// Two container files hold those same records:
//   - one_per_block: hand-framed, one record per block (10000 blocks). At
//     6 bytes per record, avro-cpp's writer cannot produce this (its
//     minimum 32-byte sync interval packs ~6 records per block), so the
//     framing is written directly.
//   - packed: avro-cpp's default 16 KiB sync interval.
// Per-block cost is (t_one - t_packed) / (blocks_one - blocks_packed).
//
// The `contiguous` rows are NOT an Avro comparison: they walk a
// std::vector of the same two fields. They exist to show what "native C++
// traversing a contiguous pointer" actually costs.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
const std::string kRecordBytes = std::string("\x08test\x32", 6);

const char* kSchemaJson =
    R"({"type":"record","name":"UserRecord","fields":[)"
    R"({"name":"username","type":"string"},)"
    R"({"name":"age","type":"int"}]})";

void Require(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FATAL: %s\n", what);
    std::exit(1);
  }
}

void PutLong(std::string& out, int64_t n) {
  uint64_t zigzag = (static_cast<uint64_t>(n) << 1) ^
                    static_cast<uint64_t>(n >> 63);
  do {
    uint8_t byte = zigzag & 0x7f;
    zigzag >>= 7;
    if (zigzag != 0) byte |= 0x80;
    out.push_back(static_cast<char>(byte));
  } while (zigzag != 0);
}

void PutString(std::string& out, const std::string& s) {
  PutLong(out, static_cast<int64_t>(s.size()));
  out += s;
}

std::string BuildHeader(const std::string& marker) {
  std::string out("Obj\x01", 4);
  PutLong(out, 2);  // metadata map: two entries, then a zero-length block
  PutString(out, "avro.schema");
  PutString(out, kSchemaJson);
  PutString(out, "avro.codec");
  PutString(out, "null");
  PutLong(out, 0);
  out += marker;
  return out;
}

// One record per block: [count=1][size=6][6 bytes][16-byte sync].
std::string BuildOnePerBlock(const std::string& marker) {
  std::string out = BuildHeader(marker);
  for (size_t i = 0; i < kCount; ++i) {
    PutLong(out, 1);
    PutLong(out, static_cast<int64_t>(kRecordBytes.size()));
    out += kRecordBytes;
    out += marker;
  }
  return out;
}

// records_per_block records per block (last block may be short), same
// kCount total records and marker as everything else. Used to sweep block
// count at fixed total record count and fixed total data size, to check
// whether per-block cost is really linear in block count (claim 3 infers
// it from exactly two points that also differ in total file size).
std::string BuildNPerBlock(const std::string& marker, size_t records_per_block) {
  std::string out = BuildHeader(marker);
  size_t remaining = kCount;
  while (remaining > 0) {
    size_t n = std::min(remaining, records_per_block);
    PutLong(out, static_cast<int64_t>(n));
    PutLong(out, static_cast<int64_t>(n * kRecordBytes.size()));
    for (size_t i = 0; i < n; ++i) out += kRecordBytes;
    out += marker;
    remaining -= n;
  }
  return out;
}

std::string WriteCppPacked(const avro::ValidSchema& schema,
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

// Blocks = occurrences of the 16-byte sync marker, minus the header's own.
size_t CountBlocks(const std::string& file) {
  const std::string marker = file.substr(file.size() - 16);
  size_t hits = 0;
  for (size_t pos = file.find(marker); pos != std::string::npos;
       pos = file.find(marker, pos + 16)) {
    ++hits;
  }
  Require(hits >= 1, "sync marker not found");
  return hits - 1;
}

size_t OursRead(const std::string& bytes) {
  auto reader = bridge::DataFileReader::FromBytes(bytes);
  Require(reader.ok(), "ours: FromBytes");
  size_t count = 0;
  while (true) {
    auto ready = reader->NextReady();
    Require(ready.ok(), "ours: NextReady");
    if (!*ready) break;
    auto value = reader->NextValue();
    Require(value.ok(), "ours: NextValue");
    benchmark::DoNotOptimize(&*value);
    ++count;
  }
  return count;
}

// Same work without the NextReady call: NextValue drives the parser itself
// and reports the clean end as kOutOfRange, halving the FFI crossings.
size_t OursReadSingleCall(const std::string& bytes) {
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

struct User {
  std::string username;
  int32_t age;
};

}  // namespace

int main(int argc, char** argv) {
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) return 1;

  const avro::ValidSchema cpp_schema =
      avro::compileJsonSchemaFromString(kSchemaJson);

  std::vector<avro::GenericDatum> cpp_values;
  static std::vector<User> contiguous;
  for (size_t i = 0; i < kCount; ++i) {
    avro::GenericDatum datum(cpp_schema);
    auto& rec = datum.value<avro::GenericRecord>();
    rec.fieldAt(0) = avro::GenericDatum(std::string("test"));
    rec.fieldAt(1) = avro::GenericDatum(static_cast<int32_t>(25));
    cpp_values.push_back(std::move(datum));
    contiguous.push_back(User{"test", 25});
  }

  static const std::string packed = WriteCppPacked(cpp_schema, cpp_values);
  static const std::string one_per_block =
      BuildOnePerBlock(packed.substr(packed.size() - 16));

  // The records really are the reported 6 bytes: the first block of the
  // packed file must begin with them, repeated.
  const size_t data_start = packed.find(kRecordBytes + kRecordBytes);
  Require(data_start != std::string::npos,
          "packed file does not contain repeated \\x08test\\x32 records");

  const size_t blocks_one = CountBlocks(one_per_block);
  const size_t blocks_packed = CountBlocks(packed);
  std::fprintf(stderr,
               "one_per_block: %zu blocks, %zu bytes (%.1f%% framing) | "
               "packed: %zu blocks, %zu bytes | %zu records x 6 bytes\n",
               blocks_one, one_per_block.size(),
               100.0 * (1.0 - 60000.0 / one_per_block.size()), blocks_packed,
               packed.size(), kCount);
  Require(OursRead(one_per_block) == kCount, "ours: short read (one)");
  Require(OursRead(packed) == kCount, "ours: short read (packed)");
  Require(OursReadSingleCall(packed) == kCount, "ours: short read (1call)");
  Require(CppRead(one_per_block) == kCount, "cpp: short read (one)");
  Require(CppRead(packed) == kCount, "cpp: short read (packed)");

  for (const auto& [label, file] :
       {std::pair<const char*, const std::string*>{"one_per_block",
                                                   &one_per_block},
        std::pair<const char*, const std::string*>{"packed", &packed}}) {
    benchmark::RegisterBenchmark(
        std::string("ours/read/") + label,
        [file](benchmark::State& state) {
          for (auto _ : state) Require(OursRead(*file) == kCount, "short");
          state.SetItemsProcessed(state.iterations() * kCount);
        });
    benchmark::RegisterBenchmark(
        std::string("avrocpp/read/") + label,
        [file](benchmark::State& state) {
          for (auto _ : state) Require(CppRead(*file) == kCount, "short");
          state.SetItemsProcessed(state.iterations() * kCount);
        });
  }
  benchmark::RegisterBenchmark(
      "ours/read_no_nextready/packed", [](benchmark::State& state) {
        for (auto _ : state)
          Require(OursReadSingleCall(packed) == kCount, "short");
        state.SetItemsProcessed(state.iterations() * kCount);
      });

  // Block-count sweep at fixed total record count and (nearly) fixed total
  // data size: claim 3 infers a per-block cost from exactly two file shapes
  // that differ in block count AND total size AND cache footprint. This
  // sweeps records-per-block so the fit has more than two points and the
  // file-size confound is minimized (framing overhead per block is only
  // ~18 bytes, dwarfed by the payload except at the extreme end).
  static const std::vector<size_t> kRecordsPerBlock = {1,   2,   5,    10,
                                                        25,  50,  100,  250,
                                                        500, 1000, 2500, 10000};
  static std::vector<std::string> sweep_files;
  for (size_t rpb : kRecordsPerBlock) {
    sweep_files.push_back(
        BuildNPerBlock(packed.substr(packed.size() - 16), rpb));
  }
  for (size_t idx = 0; idx < kRecordsPerBlock.size(); ++idx) {
    const std::string* file = &sweep_files[idx];
    size_t rpb = kRecordsPerBlock[idx];
    size_t blocks = (kCount + rpb - 1) / rpb;
    Require(OursRead(*file) == kCount, "sweep: ours short read");
    Require(CppRead(*file) == kCount, "sweep: cpp short read");
    char name[64];
    std::snprintf(name, sizeof(name), "ours/read/sweep_b%06zu", blocks);
    benchmark::RegisterBenchmark(name, [file](benchmark::State& state) {
      for (auto _ : state) Require(OursRead(*file) == kCount, "short");
      state.SetItemsProcessed(state.iterations() * kCount);
    });
    std::snprintf(name, sizeof(name), "avrocpp/read/sweep_b%06zu", blocks);
    benchmark::RegisterBenchmark(name, [file](benchmark::State& state) {
      for (auto _ : state) Require(CppRead(*file) == kCount, "short");
      state.SetItemsProcessed(state.iterations() * kCount);
    });
  }

  // Isolates the setup cost of opening a reader (no records drained) to
  // quantify how much of ours/read is the full-buffer copy that
  // DataFileReader::feed() performs (Vec::extend_from_slice) versus
  // avrocpp's zero-copy memoryInputStream wrap.
  benchmark::RegisterBenchmark(
      "ours/construct_only/packed", [](benchmark::State& state) {
        for (auto _ : state) {
          auto reader = bridge::DataFileReader::FromBytes(packed);
          Require(reader.ok(), "construct_only: FromBytes");
          benchmark::DoNotOptimize(&*reader);
        }
      });
  benchmark::RegisterBenchmark(
      "avrocpp/construct_only/packed", [](benchmark::State& state) {
        for (auto _ : state) {
          auto in = avro::memoryInputStream(
              reinterpret_cast<const uint8_t*>(packed.data()), packed.size());
          avro::DataFileReader<avro::GenericDatum> reader(std::move(in));
          benchmark::DoNotOptimize(&reader);
        }
      });

  // Raw copy cost at the same file size, to compare against the delta
  // between ours/construct_only and avrocpp/construct_only above.
  benchmark::RegisterBenchmark(
      "raw/memcpy/packed_size", [](benchmark::State& state) {
        std::vector<uint8_t> dst;
        for (auto _ : state) {
          dst.clear();
          dst.reserve(packed.size());
          std::memcpy(dst.data(), packed.data(), packed.size());
          benchmark::DoNotOptimize(dst.data());
        }
      });

  // A 10 MB buffer (the scale of avro_benchmark.cc's `strings` dataset,
  // 10000 records x ~1 KiB) to show how the copy cost scales with file
  // size rather than staying pinned to the 60 KB block_probe file.
  static const std::vector<uint8_t> big_buf(10 * 1024 * 1024, 0x42);
  benchmark::RegisterBenchmark(
      "raw/memcpy/10MB", [](benchmark::State& state) {
        std::vector<uint8_t> dst;
        for (auto _ : state) {
          dst.clear();
          dst.reserve(big_buf.size());
          std::memcpy(dst.data(), big_buf.data(), big_buf.size());
          benchmark::DoNotOptimize(dst.data());
        }
      });

  // Not Avro: a pointer walk over the same fields.
  benchmark::RegisterBenchmark(
      "contiguous/traverse/struct_array", [](benchmark::State& state) {
        for (auto _ : state) {
          int64_t sum = 0;
          for (const User& user : contiguous)
            sum += user.age + user.username[0];
          benchmark::DoNotOptimize(sum);
        }
        state.SetItemsProcessed(state.iterations() * kCount);
      });

  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
