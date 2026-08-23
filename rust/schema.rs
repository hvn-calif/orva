//! Crubit-friendly wrapper around `apache_avro::Schema`.

use crate::make_vec_type;
use crate::vec_u8::{catch_panic, utf8, Status, VecU8};
use apache_avro::rabin::Rabin;
use apache_avro::schema_compatibility::SchemaCompatibility;
use apache_avro::Schema;
use md5::Md5;
use sha2::Sha256;
use std::sync::OnceLock;

/// Wrapper around an Avro schema. Replaces avrocpp's `ValidSchema`.
#[derive(Debug, Clone, PartialEq)]
pub struct AvroSchema {
    pub(crate) schema: Schema,
}

impl Default for AvroSchema {
    fn default() -> Self {
        AvroSchema { schema: Schema::Null }
    }
}

/// Whether parsing rejects text after the schema's JSON document.
///
/// Off by default, which is avrocpp's behaviour: its JSON reader stops once it
/// has one complete value and never looks at what follows, so `"int"` followed
/// by anything is still the schema `"int"`. Off also matters more than it looks,
/// because a schema is a cache key and a fingerprint input: a producer on
/// avrocpp can write a document with a NUL pad, a stray byte or a second
/// document behind it, and rejecting those makes data that avrocpp reads
/// unreadable here. Trailing whitespace was never at issue, since serde_json
/// accepts it either way.
///
/// On, the input must be one JSON document and nothing else, which is what
/// serde_json enforces and what this binding shipped with. Text after a
/// complete document usually does mean a truncated write or two documents
/// concatenated by mistake, so it stays available to whoever wants it.
static REJECT_TEXT_AFTER_SCHEMA_JSON: OnceLock<bool> = OnceLock::new();

const DEFAULT_REJECT_TEXT_AFTER_SCHEMA_JSON: bool = false;

/// Sets whether [`AvroSchema::parse`] and [`AvroSchema::parse_list`] reject text
/// after the schema's JSON document. Process-global and first-call-wins, like
/// [`crate::datum::set_max_allocation_bytes`], and read on the first parse, so
/// call it before parsing anything. Returns the setting actually in effect,
/// which differs from the argument if something already set it.
pub fn set_reject_text_after_schema_json(reject: bool) -> bool {
    *REJECT_TEXT_AFTER_SCHEMA_JSON.get_or_init(|| reject)
}

fn reject_text_after_schema_json() -> bool {
    *REJECT_TEXT_AFTER_SCHEMA_JSON.get_or_init(|| DEFAULT_REJECT_TEXT_AFTER_SCHEMA_JSON)
}

/// `raw` cut at the end of its first complete JSON document, when anything
/// follows it. `None` when the document already runs to the end of `raw`, when
/// there is no complete document to find, or when the caller asked for the text
/// after it to be rejected.
///
/// Cuts bytes rather than a `&str` because two of the three ways this binding
/// used to refuse such an input happen before any JSON parsing: `parse`
/// validates the whole buffer as UTF-8 first, so invalid bytes *after* a
/// perfectly good schema were enough to lose it.
fn json_document_prefix(raw: &[u8]) -> Option<&[u8]> {
    if reject_text_after_schema_json() {
        return None;
    }
    let mut documents =
        serde_json::Deserializer::from_slice(raw).into_iter::<serde_json::Value>();
    documents.next()?.ok()?;
    let end = documents.byte_offset();
    if end < raw.len() { Some(&raw[..end]) } else { None }
}

/// Parses one schema from bytes that must be entirely one JSON document.
fn parse_document(raw_json: &[u8]) -> Result<AvroSchema, VecU8> {
    let json = utf8(raw_json)?;
    match Schema::parse_str(json) {
        Ok(schema) => Ok(AvroSchema { schema }),
        Err(err) => Err(err.to_string().into()),
    }
}

/// Parses cross-referencing schemas from buffers that must each be entirely one
/// JSON document.
fn parse_document_list(raw_jsons: &[&[u8]]) -> Result<VecAvroSchema, VecU8> {
    let mut jsons = Vec::with_capacity(raw_jsons.len());
    for raw_json in raw_jsons {
        jsons.push(utf8(raw_json)?);
    }
    match Schema::parse_list(jsons) {
        Ok(schemas) => Ok(schemas
            .into_iter()
            .map(|schema| AvroSchema { schema })
            .collect::<Vec<AvroSchema>>()
            .into()),
        Err(err) => Err(err.to_string().into()),
    }
}

impl AvroSchema {
    /// Parses an Avro schema from its JSON representation.
    ///
    /// Text after the schema's JSON document is ignored, as avrocpp ignores it,
    /// unless [`set_reject_text_after_schema_json`] asked otherwise. The retry
    /// on a shorter input runs only when the whole buffer failed, so an input
    /// that parses today cannot change meaning and pays nothing.
    pub fn parse(raw_json: &[u8]) -> Result<AvroSchema, VecU8> {
        catch_panic(|| match parse_document(raw_json) {
            Ok(schema) => Ok(schema),
            Err(err) => match json_document_prefix(raw_json) {
                Some(prefix) => parse_document(prefix),
                None => Err(err),
            },
        })
    }

    /// Parses a list of schemas that may reference each other by name.
    /// The returned schemas are in the same order as the input.
    ///
    /// Text after each schema's JSON document is ignored, as in [`Self::parse`].
    pub fn parse_list(raw_jsons: &[VecU8]) -> Result<VecAvroSchema, VecU8> {
        catch_panic(|| {
            let raws: Vec<&[u8]> = raw_jsons.iter().map(|raw| raw.as_slice()).collect();
            let parsed = parse_document_list(&raws);
            if parsed.is_ok() {
                return parsed;
            }
            let prefixes: Vec<&[u8]> = raws
                .iter()
                .map(|raw| json_document_prefix(raw).unwrap_or(raw))
                .collect();
            if prefixes == raws {
                return parsed;
            }
            parse_document_list(&prefixes)
        })
    }

    /// Returns the Parsing Canonical Form of this schema, as defined by the
    /// Avro specification.
    pub fn canonical_form(&self) -> VecU8 {
        self.schema.canonical_form().into()
    }

    /// Returns the CRC-64-AVRO (Rabin) fingerprint of the canonical form as
    /// a signed 64-bit integer, matching the value produced by the Java
    /// implementation's `SchemaNormalization.parsingFingerprint64`.
    pub fn fingerprint_rabin(&self) -> i64 {
        let fingerprint = self.schema.fingerprint::<Rabin>();
        match <[u8; 8]>::try_from(fingerprint.bytes.as_slice()) {
            Ok(bytes) => i64::from_le_bytes(bytes),
            // Loud rather than silently wrong: returning a made-up
            // fingerprint (e.g. 0) would be undetectable by callers.
            Err(_) => unreachable!("Rabin digest is always 8 bytes, little-endian"),
        }
    }

    /// Returns the Rabin fingerprint as a lowercase hex string
    /// (little-endian byte order, as rendered by apache-avro).
    pub fn fingerprint_rabin_hex(&self) -> VecU8 {
        self.schema.fingerprint::<Rabin>().to_string().into()
    }

    /// Returns the MD5 fingerprint of the canonical form as a lowercase hex
    /// string.
    pub fn fingerprint_md5_hex(&self) -> VecU8 {
        self.schema.fingerprint::<Md5>().to_string().into()
    }

    /// Returns the SHA-256 fingerprint of the canonical form as a lowercase
    /// hex string.
    pub fn fingerprint_sha256_hex(&self) -> VecU8 {
        self.schema.fingerprint::<Sha256>().to_string().into()
    }

    /// Returns the name of a named schema (record, enum, fixed).
    /// Returns an error for unnamed schemas.
    pub fn name(&self) -> Result<VecU8, VecU8> {
        match self.schema.name() {
            Some(name) => Ok(name.name.clone().into()),
            None => Err("Schema is not a named schema".into()),
        }
    }

    /// Returns the namespace of a named schema, or an empty string if the
    /// schema has no namespace. Returns an error for unnamed schemas.
    /// (Named `namespace_name` because `namespace` is a C++ keyword.)
    pub fn namespace_name(&self) -> Result<VecU8, VecU8> {
        match self.schema.name() {
            Some(name) => Ok(name.namespace.clone().unwrap_or_default().into()),
            None => Err("Schema is not a named schema".into()),
        }
    }

    /// Returns the full name (namespace.name) of a named schema.
    /// Returns an error for unnamed schemas.
    pub fn full_name(&self) -> Result<VecU8, VecU8> {
        match self.schema.name() {
            Some(name) => Ok(name.fullname(None).into()),
            None => Err("Schema is not a named schema".into()),
        }
    }

    /// Returns the JSON representation of this schema.
    pub fn to_json_string(&self) -> Result<VecU8, VecU8> {
        match serde_json::to_string(&self.schema) {
            Ok(json) => Ok(json.into()),
            Err(err) => Err(err.to_string().into()),
        }
    }

    /// Returns the name of the schema type, e.g. "record" or "int".
    /// Logical types report their underlying name, e.g. "timestamp-millis".
    pub fn type_name(&self) -> VecU8 {
        let name = match &self.schema {
            Schema::Null => "null",
            Schema::Boolean => "boolean",
            Schema::Int => "int",
            Schema::Long => "long",
            Schema::Float => "float",
            Schema::Double => "double",
            Schema::Bytes => "bytes",
            Schema::String => "string",
            Schema::Record(_) => "record",
            Schema::Enum(_) => "enum",
            Schema::Array(_) => "array",
            Schema::Map(_) => "map",
            Schema::Union(_) => "union",
            Schema::Fixed(_) => "fixed",
            Schema::Decimal(_) => "decimal",
            Schema::BigDecimal => "big-decimal",
            Schema::Uuid => "uuid",
            Schema::Date => "date",
            Schema::TimeMillis => "time-millis",
            Schema::TimeMicros => "time-micros",
            Schema::TimestampMillis => "timestamp-millis",
            Schema::TimestampMicros => "timestamp-micros",
            Schema::TimestampNanos => "timestamp-nanos",
            Schema::LocalTimestampMillis => "local-timestamp-millis",
            Schema::LocalTimestampMicros => "local-timestamp-micros",
            Schema::LocalTimestampNanos => "local-timestamp-nanos",
            Schema::Duration => "duration",
            Schema::Ref { .. } => "ref",
        };
        name.into()
    }

    pub fn is_null(&self) -> bool {
        matches!(self.schema, Schema::Null)
    }

    pub fn is_boolean(&self) -> bool {
        matches!(self.schema, Schema::Boolean)
    }

    pub fn is_int(&self) -> bool {
        matches!(self.schema, Schema::Int)
    }

    pub fn is_long(&self) -> bool {
        matches!(self.schema, Schema::Long)
    }

    pub fn is_float(&self) -> bool {
        matches!(self.schema, Schema::Float)
    }

    pub fn is_double(&self) -> bool {
        matches!(self.schema, Schema::Double)
    }

    pub fn is_bytes(&self) -> bool {
        matches!(self.schema, Schema::Bytes)
    }

    pub fn is_string(&self) -> bool {
        matches!(self.schema, Schema::String)
    }

    pub fn is_record(&self) -> bool {
        matches!(self.schema, Schema::Record(_))
    }

    pub fn is_enum(&self) -> bool {
        matches!(self.schema, Schema::Enum(_))
    }

    pub fn is_array(&self) -> bool {
        matches!(self.schema, Schema::Array(_))
    }

    pub fn is_map(&self) -> bool {
        matches!(self.schema, Schema::Map(_))
    }

    pub fn is_union(&self) -> bool {
        matches!(self.schema, Schema::Union(_))
    }

    pub fn is_fixed(&self) -> bool {
        matches!(self.schema, Schema::Fixed(_))
    }

    /// Checks that data written with the `writer` schema can be read using
    /// this schema (schema evolution compatibility). Returns an error with
    /// the incompatibility reason otherwise.
    pub fn can_read_from(&self, writer: &AvroSchema) -> Status {
        match SchemaCompatibility::can_read(&writer.schema, &self.schema) {
            Ok(()) => Ok(0),
            Err(err) => Err(err.to_string().into()),
        }
    }

    /// Checks that data written with either schema can be read with the
    /// other one.
    pub fn mutual_read(&self, other: &AvroSchema) -> Status {
        match SchemaCompatibility::mutual_read(&other.schema, &self.schema) {
            Ok(()) => Ok(0),
            Err(err) => Err(err.to_string().into()),
        }
    }

    /// Compares two schemas for equality.
    pub fn equals(&self, other: &AvroSchema) -> bool {
        self.schema == other.schema
    }
}

make_vec_type!(AvroSchema, VecAvroSchema);

#[cfg(test)]
mod tests {
    use super::*;

    const RECORD_SCHEMA: &str = r#"{
        "type": "record",
        "name": "User",
        "namespace": "com.example",
        "fields": [
            {"name": "id", "type": "long"},
            {"name": "name", "type": "string"}
        ]
    }"#;

    #[test]
    fn parse_primitive() {
        let schema = AvroSchema::parse(b"\"int\"").unwrap();
        assert!(schema.is_int());
        assert!(!schema.is_long());
        assert_eq!(schema.type_name().as_slice(), b"int");
    }

    #[test]
    fn parse_record() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        assert!(schema.is_record());
        assert_eq!(schema.name().unwrap().as_slice(), b"User");
        assert_eq!(schema.namespace_name().unwrap().as_slice(), b"com.example");
        assert_eq!(schema.full_name().unwrap().as_slice(), b"com.example.User");
    }

    #[test]
    fn parse_malformed_fails() {
        assert!(AvroSchema::parse(b"{not json").is_err());
        assert!(AvroSchema::parse(b"\"not_a_type\"").is_err());
        // Invalid UTF-8.
        assert!(AvroSchema::parse(&[0xff, 0xfe]).is_err());
    }

    #[test]
    fn unnamed_schema_has_no_name() {
        let schema = AvroSchema::parse(b"\"int\"").unwrap();
        assert!(schema.name().is_err());
        assert!(schema.namespace_name().is_err());
        assert!(schema.full_name().is_err());
    }

    #[test]
    fn canonical_form_of_record() {
        // Test case 019 from apache/avro share/test/data/schema-tests.txt.
        let schema =
            AvroSchema::parse(br#"{"fields":[], "type":"record", "name":"foo"}"#).unwrap();
        assert_eq!(
            schema.canonical_form().as_slice(),
            br#"{"name":"foo","type":"record","fields":[]}"#
        );
    }

    #[test]
    fn rabin_fingerprints_match_apache_test_data() {
        // Expected values from apache/avro share/test/data/schema-tests.txt
        // (test cases 004 and 019).
        let int_schema = AvroSchema::parse(b"\"int\"").unwrap();
        assert_eq!(int_schema.fingerprint_rabin(), 8247732601305521295);

        let record_schema =
            AvroSchema::parse(br#"{"fields":[], "type":"record", "name":"foo"}"#).unwrap();
        assert_eq!(record_schema.fingerprint_rabin(), -4824392279771201922);
    }

    #[test]
    fn hex_fingerprints_have_expected_shape() {
        let schema = AvroSchema::parse(b"\"int\"").unwrap();
        let rabin = schema.fingerprint_rabin_hex();
        let md5 = schema.fingerprint_md5_hex();
        let sha256 = schema.fingerprint_sha256_hex();
        assert_eq!(rabin.len(), 16);
        assert_eq!(md5.len(), 32);
        assert_eq!(sha256.len(), 64);
        for digest in [rabin, md5, sha256] {
            assert!(digest.as_slice().iter().all(|b| b.is_ascii_hexdigit()));
        }
    }

    #[test]
    fn parse_list_resolves_cross_references() {
        let dependency = VecU8::from(
            r#"{"type": "record", "name": "Address", "fields": [
                {"name": "city", "type": "string"}]}"#,
        );
        let dependent = VecU8::from(
            r#"{"type": "record", "name": "Person", "fields": [
                {"name": "address", "type": "Address"}]}"#,
        );
        let schemas = AvroSchema::parse_list(&[dependency, dependent]).unwrap();
        assert_eq!(schemas.len(), 2);
        assert_eq!(schemas.as_slice()[0].name().unwrap().as_slice(), b"Address");
        assert_eq!(schemas.as_slice()[1].name().unwrap().as_slice(), b"Person");
    }

    // C1. avrocpp's JSON reader stops at the end of the first value and never
    // looks at what follows, so a schema with anything behind it is still that
    // schema. These are the three ways this binding used to refuse one: bad
    // JSON, invalid UTF-8, and a UTF-8 sequence cut short. The last two never
    // reached the JSON parser, because `parse` validated the whole buffer as
    // UTF-8 first, which is why the cut is made on bytes.
    //
    // Only the default is testable here. The setting is a `OnceLock` no test can
    // reset, so its other value lives in `avro_bridge_strict_test`.
    #[test]
    fn text_after_the_schema_json_is_ignored() {
        let clean = AvroSchema::parse(b"\"string\"").unwrap();
        for raw in [
            b"\"string\"!".as_slice(),
            b"\"string\" \n]".as_slice(),
            b"\"string\"\"".as_slice(),
            b"\"string\"f_\xc3\x28y".as_slice(),
            b"\"string\"l5\xe2\x82".as_slice(),
        ] {
            assert_eq!(AvroSchema::parse(raw).unwrap(), clean);
        }
    }

    // The cut is at the end of the first JSON document, not at the first byte
    // that looks like the end of one: a brace inside a string, and an escaped
    // quote, both stay inside the string.
    #[test]
    fn text_after_the_schema_json_respects_string_literals() {
        const BRACES_IN_A_STRING: &str =
            r#"{"type":"record","name":"R","doc":"} \" }","fields":[]}"#;
        let clean = AvroSchema::parse(BRACES_IN_A_STRING.as_bytes()).unwrap();
        let trailing =
            AvroSchema::parse(format!("{BRACES_IN_A_STRING}!").as_bytes()).unwrap();
        assert_eq!(trailing, clean);
        assert_eq!(clean.name().unwrap().as_slice(), b"R");
    }

    // Ignoring text after a complete document and accepting a document cut
    // short are different things: there is no complete document to fall back to
    // here, so these stay errors.
    #[test]
    fn a_truncated_schema_is_still_rejected() {
        assert!(AvroSchema::parse(br#"{"type":"reco"#).is_err());
        assert!(AvroSchema::parse(b"[\"int\"").is_err());
        assert!(AvroSchema::parse(b"").is_err());
    }

    // A complete JSON document that is not a legal schema reports why, rather
    // than reporting the trailing text that is no longer the problem. The two
    // messages match because the second parse sees exactly the first input.
    #[test]
    fn an_illegal_schema_reports_its_own_error() {
        let plain = AvroSchema::parse(br#"{"type":"nonsense"}"#).unwrap_err();
        let trailing = AvroSchema::parse(br#"{"type":"nonsense"}!"#).unwrap_err();
        assert_eq!(plain.as_slice(), trailing.as_slice());
    }

    #[test]
    fn text_after_each_schema_in_a_list_is_ignored() {
        let dependency = VecU8::from(
            r#"{"type": "record", "name": "Address", "fields": [
                {"name": "city", "type": "string"}]}!"#,
        );
        let dependent = VecU8::from(
            r#"{"type": "record", "name": "Person", "fields": [
                {"name": "address", "type": "Address"}]} {"stray": 1}"#,
        );
        let schemas = AvroSchema::parse_list(&[dependency, dependent]).unwrap();
        assert_eq!(schemas.len(), 2);
        assert_eq!(schemas.as_slice()[0].name().unwrap().as_slice(), b"Address");
        assert_eq!(schemas.as_slice()[1].name().unwrap().as_slice(), b"Person");
    }

    // The retry parses the same list again with each document cut, so an error
    // that has nothing to do with trailing text survives it.
    #[test]
    fn a_duplicate_name_in_a_list_is_still_rejected() {
        let first = VecU8::from(r#"{"type":"record","name":"R","fields":[]}"#);
        let second = VecU8::from(r#"{"type":"record","name":"R","fields":[]} !"#);
        assert!(AvroSchema::parse_list(&[first, second]).is_err());
    }

    #[test]
    fn can_read_promotion() {
        let int_schema = AvroSchema::parse(b"\"int\"").unwrap();
        let long_schema = AvroSchema::parse(b"\"long\"").unwrap();
        // int promotes to long: a long reader can read int data.
        assert!(long_schema.can_read_from(&int_schema).is_ok());
        // The reverse is not allowed.
        assert!(int_schema.can_read_from(&long_schema).is_err());
        assert!(long_schema.mutual_read(&int_schema).is_err());
        assert!(long_schema.mutual_read(&long_schema).is_ok());
    }

    #[test]
    fn to_json_string_roundtrips() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let json = schema.to_json_string().unwrap();
        let reparsed = AvroSchema::parse(json.as_slice()).unwrap();
        assert!(schema.equals(&reparsed));
    }

    #[test]
    fn equals_distinguishes_schemas() {
        let a = AvroSchema::parse(b"\"int\"").unwrap();
        let b = AvroSchema::parse(b"\"int\"").unwrap();
        let c = AvroSchema::parse(b"\"long\"").unwrap();
        assert!(a.equals(&b));
        assert!(!a.equals(&c));
    }
}
