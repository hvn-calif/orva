//! Verifies that `set_max_allocation_bytes` actually bounds decode
//! allocations on untrusted input -- the property the security review found
//! untested (the existing unit test only checked the "first call wins"
//! invariant, not that a low limit rejects oversized input).
//!
//! This lives in its own integration test, i.e. its own process, because
//! the underlying apache-avro limit is a one-shot process global: it must be
//! set before the first decode and cannot be changed afterward. Sharing a
//! process with other decode tests would make the outcome order-dependent.

use rust::datum::{decode_datum, encode_datum, set_max_allocation_bytes};
use rust::schema::AvroSchema;
use rust::value::AvroValue;

#[test]
fn max_allocation_bytes_rejects_oversized_field() {
    // Must be set before the first decode in this process.
    const LIMIT: usize = 1024;
    assert_eq!(
        set_max_allocation_bytes(LIMIT),
        LIMIT,
        "limit should take on first set"
    );

    let schema = AvroSchema::parse(b"\"bytes\"").unwrap();

    // A 64 KiB bytes field: its length prefix far exceeds the 1 KiB cap, so
    // the decoder must reject it before allocating, rather than honoring an
    // attacker-controlled length.
    let oversized = AvroValue::create_bytes(&vec![0u8; 64 * 1024]);
    let encoded = encode_datum(&schema, &oversized).unwrap();
    assert!(
        decode_datum(&schema, encoded.as_slice()).is_err(),
        "decode of an over-cap length prefix must be rejected"
    );

    // A field within the cap still decodes correctly.
    let small = AvroValue::create_bytes(b"hello");
    let encoded_small = encode_datum(&schema, &small).unwrap();
    let decoded = decode_datum(&schema, encoded_small.as_slice()).unwrap();
    assert_eq!(decoded.get_bytes().unwrap().as_slice(), b"hello");
}
