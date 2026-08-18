//! The rejecting half of `set_reject_trailing_bytes`. Its own binary because the
//! setting is a `OnceLock`: no test can reset it, so each value needs a process.
//! The default-off half lives in `datum.rs`'s unit tests.
//!
//! Mirrors how apache-avro tests `serde_human_readable`, and how this repo's
//! patches test `set_non_utf8_string_as_bytes` and `set_uuid_as_string`.
//!
//! Every test here must want the setting **on**. One that wanted it off would see
//! whatever ran first and pass or fail by scheduling, so it needs a third binary
//! rather than a place in this one. The first assertion below is what catches that
//! mistake: it fails if anything set the value to `false` before it looked.

use rust::datum::{decode_datum, encode_datum, set_reject_trailing_bytes};
use rust::schema::AvroSchema;
use rust::value::AvroValue;

#[test]
fn trailing_bytes_are_rejected_once_the_caller_asks() {
    assert!(
        set_reject_trailing_bytes(true),
        "nothing else in this binary may set this first"
    );

    let schema = AvroSchema::parse(b"\"int\"").unwrap();
    let mut buf = encode_datum(&schema, &AvroValue::create_int(7)).unwrap().into_vec();
    buf.extend_from_slice(b"\xde\xad\xbe\xef");

    let err = decode_datum(&schema, &buf).unwrap_err();
    let message = String::from_utf8(err.into_vec()).unwrap();
    assert!(message.contains("trailing bytes"), "unexpected error: {message}");

    // A correctly framed datum still decodes, so the check is on the leftovers
    // rather than on the decode.
    let clean = encode_datum(&schema, &AvroValue::create_int(7)).unwrap();
    assert_eq!(decode_datum(&schema, clean.as_slice()).unwrap().get_int().unwrap(), 7);
}

/// A later call with the opposite argument must not change the setting.
///
/// Checking only the return value would not prove that: `set(true)` then
/// `set(false)` returns `true` even for a plain mutable global with no
/// first-call-wins semantics. So this asserts the *decode behaviour* after the
/// second call, which is the thing a caller would actually be surprised by.
#[test]
fn a_later_call_cannot_turn_the_setting_off() {
    assert!(set_reject_trailing_bytes(true));
    assert!(set_reject_trailing_bytes(false), "a later call changed the setting");

    let schema = AvroSchema::parse(b"\"int\"").unwrap();
    let mut buf = encode_datum(&schema, &AvroValue::create_int(1)).unwrap().into_vec();
    buf.push(0xff);
    assert!(
        decode_datum(&schema, &buf).is_err(),
        "the decode stopped rejecting, so the second call did change the setting"
    );
}
