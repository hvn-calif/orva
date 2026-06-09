#include <iostream>
#include <string>

#include "avro_bridge.h"

// Example: the typical avrocpp workflow (compile a schema, build generic
// datums, write an object container file, read it back) on top of the
// memory-safe Rust implementation.
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

  // Build two records.
  auto writer_or = security::avro::DataFileWriter::Create(
      schema, security::avro::Codec::kDeflate);
  if (!writer_or.ok()) {
    std::cerr << "Failed to create writer: " << writer_or.status()
              << std::endl;
    return 1;
  }
  auto& writer = *writer_or;

  for (int i = 0; i < 2; ++i) {
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
  }

  auto bytes_or = writer.ToBytes();
  if (!bytes_or.ok()) {
    std::cerr << "ToBytes failed: " << bytes_or.status() << std::endl;
    return 1;
  }
  std::cout << "Container file size: " << bytes_or->size() << " bytes"
            << std::endl;

  // Read the container file back.
  auto reader_or = security::avro::DataFileReader::FromBytes(*bytes_or);
  if (!reader_or.ok()) {
    std::cerr << "FromBytes failed: " << reader_or.status() << std::endl;
    return 1;
  }
  auto& reader = *reader_or;
  std::cout << "Values in file: " << reader.Count() << std::endl;

  while (reader.HasNext()) {
    auto value_or = reader.NextValue();
    if (!value_or.ok()) {
      std::cerr << "NextValue failed: " << value_or.status() << std::endl;
      return 1;
    }
    auto json_or = value_or->ToJsonString();
    std::cout << "Record: " << (json_or.ok() ? *json_or : "<error>")
              << std::endl;
  }

  return 0;
}
