//! Demonstrates that streaming container IO bounds the working set: a large
//! file is written by draining encoded bytes incrementally (the writer never
//! holds the whole file) and read back one value at a time (the reader never
//! materializes every decoded value at once). Bounded magnitude, CI-safe.

use rust::container::{AvroCodec, StreamingDataFileReader, StreamingDataFileWriter};
use rust::schema::AvroSchema;
use rust::value::AvroValue;

const SCHEMA: &str = r#"{"type":"record","name":"M","fields":[{"name":"i","type":"long"}]}"#;

fn record(i: i64) -> AvroValue {
    let mut r = AvroValue::create_record();
    r.record_put(b"i", &AvroValue::create_long(i)).unwrap();
    r
}

#[test]
fn streaming_bounds_working_set_for_large_files() {
    let schema = AvroSchema::parse(SCHEMA.as_bytes()).unwrap();
    const N: i64 = 200_000;

    // Write: append + drain after each record. The writer auto-flushes ~16 KiB
    // blocks, so each drained chunk stays near one block rather than growing to
    // the whole file.
    let mut writer = StreamingDataFileWriter::create(&schema, AvroCodec::Null).unwrap();
    let mut file = Vec::new();
    let mut max_chunk = 0usize;
    for i in 0..N {
        writer.append(&record(i)).unwrap();
        let chunk = writer.take_bytes().unwrap();
        max_chunk = max_chunk.max(chunk.len());
        file.extend_from_slice(chunk.as_slice());
    }
    file.extend_from_slice(writer.finish().unwrap().as_slice());

    // No single incremental drain holds anywhere near the whole file: the
    // writer's working set is bounded to roughly one block.
    assert!(max_chunk < 128 * 1024, "drain chunk grew to {max_chunk} bytes");
    assert!(
        file.len() > 4 * max_chunk.max(1),
        "file ({} bytes) should span many drains (max chunk {max_chunk})",
        file.len()
    );

    // Read: one value at a time, never materializing all N decoded values.
    let mut reader = StreamingDataFileReader::from_bytes(&file).unwrap();
    let mut expected = 0i64;
    while reader.has_next() {
        let value = reader.next_value().unwrap();
        assert_eq!(value.get_record_field(b"i").unwrap().get_long().unwrap(), expected);
        expected += 1;
    }
    assert_eq!(expected, N);
}
