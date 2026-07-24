#include <iostream>
#include <string>

#include "avro_bridge.h"

// Example: the typical avrocpp workflow (compile a schema, build generic
// datums, write an object container file, read it back) on top of the
// memory-safe Rust implementation. Both the writer and the reader stream:
// encoded bytes are drained incrementally while writing, and input is fed
// in chunks while reading, so neither side ever holds the whole file.
int main(int argc, char** argv) {
  const std::string schema_json = R"({
    "type": "record",
    "name": "Measurement",
    "namespace": "com.example",
    "fields": [
      {"name": "sensor", "type": "string"},
      {"name": "value", "type": "double"},
      {"name": "healthy", "type": "boolean"}
    ]
  })";

  auto schema_or = security::avro::AvroSchema::Parse(schema_json);
  if (!schema_or.ok()) {
    std::cerr << "Failed to parse schema: " << schema_or.status() << std::endl;
    return 1;
  }
  auto& schema = *schema_or;
  std::cout << "Schema: " << schema.FullName().value_or("?")
            << " rabin_fingerprint=" << schema.FingerprintRabin() << std::endl;

  // Write: append records, draining encoded bytes as we go. The caller owns
  // the sink (here a string; a file or socket works the same way).
  auto writer_or = security::avro::DataFileWriter::Create(
      schema, security::avro::Codec::kDeflate);
  if (!writer_or.ok()) {
    std::cerr << "Failed to create writer: " << writer_or.status()
              << std::endl;
    return 1;
  }
  auto& writer = *writer_or;

  std::string file;
  for (int i = 0; i < 3; ++i) {
    auto record = security::avro::AvroValue::CreateRecord();
    auto sensor =
        security::avro::AvroValue::CreateString(i == 0 ? "temp" : "pressure");
    if (!sensor.ok()) {
      std::cerr << "CreateString failed: " << sensor.status() << std::endl;
      return 1;
    }
    if (auto s = record.RecordPut("sensor", *sensor); !s.ok()) {
      std::cerr << "RecordPut failed: " << s << std::endl;
      return 1;
    }
    (void)record.RecordPut(
        "value", security::avro::AvroValue::CreateDouble(20.5 + i));
    (void)record.RecordPut("healthy",
                           security::avro::AvroValue::CreateBoolean(true));
    if (auto s = writer.Append(record); !s.ok()) {
      std::cerr << "Append failed: " << s << std::endl;
      return 1;
    }
    auto chunk = writer.TakeBytes();  // Drain whatever is encoded so far.
    if (!chunk.ok()) {
      std::cerr << "TakeBytes failed: " << chunk.status() << std::endl;
      return 1;
    }
    file += *chunk;
  }
  auto tail = writer.Finish();
  if (!tail.ok()) {
    std::cerr << "Finish failed: " << tail.status() << std::endl;
    return 1;
  }
  file += *tail;
  std::cout << "Container file size: " << file.size() << " bytes" << std::endl;

  // Read: feed the file in chunks (as if arriving from disk or a socket)
  // and decode values as soon as they become available.
  auto reader = security::avro::DataFileReader::Create();
  const size_t kChunkSize = 64;
  for (size_t offset = 0; offset < file.size(); offset += kChunkSize) {
    if (auto s = reader.Feed(
            absl::string_view(file).substr(offset, kChunkSize));
        !s.ok()) {
      std::cerr << "Feed failed: " << s << std::endl;
      return 1;
    }
    while (true) {
      auto ready = reader.NextReady();
      if (!ready.ok()) {
        std::cerr << "NextReady failed: " << ready.status() << std::endl;
        return 1;
      }
      if (!*ready) break;
      auto value_or = reader.NextValue();
      if (!value_or.ok()) {
        std::cerr << "NextValue failed: " << value_or.status() << std::endl;
        return 1;
      }
      auto json_or = value_or->ToJsonString();
      std::cout << "Record: " << (json_or.ok() ? *json_or : "<error>")
                << std::endl;
    }
  }
  if (auto s = reader.CloseInput(); !s.ok()) {
    std::cerr << "CloseInput failed: " << s << std::endl;
    return 1;
  }
  auto final_ready = reader.NextReady();
  if (!final_ready.ok()) {
    std::cerr << "Final NextReady failed: " << final_ready.status()
              << std::endl;
    return 1;
  }
  if (!reader.AtEnd()) {
    std::cerr << "Stream did not end cleanly" << std::endl;
    return 1;
  }
  std::cout << "Writer schema round-tripped: "
            << (reader.WriterSchema().ok() &&
                        *reader.WriterSchema() == schema
                    ? "yes"
                    : "no")
            << std::endl;

  return 0;
}
