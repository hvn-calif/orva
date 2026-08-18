//! Demonstrations of the three untrusted-input properties surfaced in the
//! security review of this binding. Two are hardenings this crate enforces
//! (panic containment is covered by unit tests);
//! one is a documented residual limitation (decompression amplification,
//! unbounded decode recursion). Each test is written to be safe to run in
//! CI -- it illustrates the issue at a bounded magnitude rather than
//! actually exhausting memory or the stack.

use rust::container::{AvroCodec, DataFileReader, DataFileWriter};
use rust::datum::{decode_datum, encode_datum};
use rust::schema::AvroSchema;
use rust::value::AvroValue;

/// Issue 1: trailing bytes after a single datum. This binding now follows
/// avrocpp and ignores them, because code being migrated may hand a padded or
/// over-allocated buffer to a decode. The rejecting behaviour is still available
/// through `set_reject_trailing_bytes`, and lives in tests/reject_trailing_bytes.rs
/// because the setting is a OnceLock and needs its own process.
///
/// What must not change is that a *truncated* datum is an error. Ignoring bytes
/// left over after a complete datum and accepting a datum with bytes missing are
/// different things, and this asserts the second is still refused.
#[test]
fn trailing_bytes_are_ignored_but_a_truncated_datum_is_not() {
    let schema = AvroSchema::parse(b"\"int\"").unwrap();

    let mut buf = encode_datum(&schema, &AvroValue::create_int(7)).unwrap().into_vec();
    buf.extend_from_slice(b"\xde\xad\xbe\xef");
    assert_eq!(decode_datum(&schema, &buf).unwrap().get_int().unwrap(), 7);

    let clean = encode_datum(&schema, &AvroValue::create_int(7)).unwrap();
    assert_eq!(decode_datum(&schema, clean.as_slice()).unwrap().get_int().unwrap(), 7);

    // A length prefix of 2 with one byte behind it.
    let string_schema = AvroSchema::parse(b"\"string\"").unwrap();
    assert!(decode_datum(&string_schema, &[0x04, 0x61]).is_err());
}

/// Issue 2 (documented limitation): compressed container files amplify a
/// tiny input into a large in-memory payload, with no cap.
///
/// apache-avro provides no decompressed-size limit and `SetMaxAllocationBytes`
/// does not cover the codec path, so a maliciously crafted file is a
/// memory-amplification DoS. Mitigate with an external memory limit
/// (RLIMIT_AS / cgroups) on untrusted input. The codec set is restricted to
/// avrocpp's (null/deflate/snappy/zstd); bzip2/xz, the worst bomb formats,
/// are intentionally unavailable. This test uses a bounded 4 MiB payload so
/// it cannot exhaust CI memory while still showing the amplification ratio.
#[test]
fn compression_amplifies_small_input_into_large_memory() {
    let schema = AvroSchema::parse(
        br#"{"type":"record","name":"Blob","fields":[{"name":"data","type":"string"}]}"#,
    )
    .unwrap();

    let payload = vec![b'A'; 4 * 1024 * 1024]; // 4 MiB, maximally compressible
    let mut record = AvroValue::create_record();
    record.record_put(b"data", &AvroValue::create_string(&payload).unwrap()).unwrap();

    let mut writer = DataFileWriter::create(&schema, AvroCodec::Deflate).unwrap();
    writer.append(&record).unwrap();
    let compressed = writer.to_bytes().unwrap();

    // Deflate collapses 4 MiB of repeated bytes to a few KiB on disk.
    assert!(
        compressed.len() < 64 * 1024,
        "expected a tiny compressed file, got {} bytes",
        compressed.len()
    );

    // Reading that tiny file reconstructs the full 4 MiB in memory, unbounded.
    let mut reader = DataFileReader::from_bytes(compressed.as_slice()).unwrap();
    let value = reader.next_value().unwrap();
    let data = value.get_record_field(b"data").unwrap().get_string().unwrap();
    assert_eq!(data.len(), payload.len());

    let amplification = data.len() / compressed.len();
    assert!(amplification > 50, "expected large amplification, got {amplification}x");
}

/// Issue 3 (documented limitation): decode recurses one frame per nesting
/// level with no depth guard, matching avrocpp (also unbounded).
///
/// A self-referential schema lets attacker-controlled *data* drive decode
/// depth, bounded only by the stack. This is a cheap DoS: on the default
/// ~2 MiB thread stack, a chain only a few hundred links deep (a few
/// hundred bytes on the wire) already overflows and aborts the process --
/// and a stack overflow is a fatal, uncatchable abort that the panic guard
/// cannot intercept. To demonstrate this safely without crashing the test
/// runner, the work runs on a thread with a generous stack: it shows the
/// library enforces NO depth limit (a deep chain round-trips given enough
/// stack), so the residual risk is the absence of a bound, not a ceiling.
#[test]
fn deeply_nested_values_decode_without_a_depth_limit() {
    std::thread::Builder::new()
        .stack_size(256 * 1024 * 1024)
        .spawn(|| {
            // A recursive schema: a singly linked list. `next` is null-or-Node.
            let schema = AvroSchema::parse(
                br#"{"type":"record","name":"Node","fields":[
                    {"name":"next","type":["null","Node"]}]}"#,
            )
            .unwrap();

            // Kept modest: in debug builds each recursive encode/decode
            // frame is very large (a few hundred levels overflow the default
            // ~2 MiB stack), so this runs on the 256 MiB stack above with a
            // generous margin. The number is illustrative; the point is that
            // the library imposes no limit of its own.
            const DEPTH: usize = 100;

            // Innermost link: next = null (union branch 0).
            let mut node = AvroValue::create_record();
            node.record_put(b"next", &AvroValue::create_union(0, &AvroValue::create_null()))
                .unwrap();
            // Wrap DEPTH times: each next = Node (union branch 1).
            for _ in 0..DEPTH {
                let mut outer = AvroValue::create_record();
                outer.record_put(b"next", &AvroValue::create_union(1, &node)).unwrap();
                node = outer;
            }

            let encoded = encode_datum(&schema, &node).unwrap();
            let decoded = decode_datum(&schema, encoded.as_slice()).unwrap();
            assert!(decoded.equals(&node));
        })
        .unwrap()
        .join()
        .unwrap();
}
