//! Crubit-friendly wrapper around `apache_avro::Schema`.

use crate::make_vec_type;
use crate::vec_u8::{Status, VecU8, catch_panic, utf8};
use apache_avro::Schema;
use apache_avro::rabin::Rabin;
use apache_avro::schema_compatibility::SchemaCompatibility;
use md5::Md5;
use sha2::Sha256;

/// The Avro schema/value variant exposed across the C++ boundary.
#[repr(C)]
#[derive(Default, Debug, Clone, Copy, PartialEq, Eq)]
pub enum SchemaType {
    #[default]
    Null,
    Boolean,
    Int,
    Long,
    Float,
    Double,
    Bytes,
    String,
    Record,
    Enum,
    Array,
    Map,
    Union,
    Fixed,
    Decimal,
    BigDecimal,
    Uuid,
    Date,
    TimeMillis,
    TimeMicros,
    TimestampMillis,
    TimestampMicros,
    TimestampNanos,
    LocalTimestampMillis,
    LocalTimestampMicros,
    LocalTimestampNanos,
    Duration,
    Ref,
}

impl SchemaType {
    pub(crate) fn name(self) -> &'static str {
        match self {
            Self::Null => "null",
            Self::Boolean => "boolean",
            Self::Int => "int",
            Self::Long => "long",
            Self::Float => "float",
            Self::Double => "double",
            Self::Bytes => "bytes",
            Self::String => "string",
            Self::Record => "record",
            Self::Enum => "enum",
            Self::Array => "array",
            Self::Map => "map",
            Self::Union => "union",
            Self::Fixed => "fixed",
            Self::Decimal => "decimal",
            Self::BigDecimal => "big-decimal",
            Self::Uuid => "uuid",
            Self::Date => "date",
            Self::TimeMillis => "time-millis",
            Self::TimeMicros => "time-micros",
            Self::TimestampMillis => "timestamp-millis",
            Self::TimestampMicros => "timestamp-micros",
            Self::TimestampNanos => "timestamp-nanos",
            Self::LocalTimestampMillis => "local-timestamp-millis",
            Self::LocalTimestampMicros => "local-timestamp-micros",
            Self::LocalTimestampNanos => "local-timestamp-nanos",
            Self::Duration => "duration",
            Self::Ref => "ref",
        }
    }
}

/// Wrapper around an Avro schema. Replaces avrocpp's `ValidSchema`.
#[derive(Debug, Clone, PartialEq)]
pub struct AvroSchema {
    pub(crate) schema: Schema,
}

impl Default for AvroSchema {
    fn default() -> Self {
        AvroSchema {
            schema: Schema::Null,
        }
    }
}

impl AvroSchema {
    /// Parses an Avro schema from its JSON representation.
    pub fn parse(raw_json: &[u8]) -> Result<AvroSchema, VecU8> {
        catch_panic(|| {
            let json = utf8(raw_json)?;
            match Schema::parse_str(json) {
                Ok(schema) => Ok(AvroSchema { schema }),
                Err(err) => Err(err.to_string().into()),
            }
        })
    }

    /// Parses a list of schemas that may reference each other by name.
    /// The returned schemas are in the same order as the input.
    pub fn parse_list(raw_jsons: &[VecU8]) -> Result<VecAvroSchema, VecU8> {
        catch_panic(|| {
            let mut jsons = Vec::with_capacity(raw_jsons.len());
            for raw_json in raw_jsons {
                jsons.push(utf8(raw_json.as_slice())?);
            }
            match Schema::parse_list(jsons) {
                Ok(schemas) => Ok(schemas
                    .into_iter()
                    .map(|schema| AvroSchema { schema })
                    .collect::<Vec<AvroSchema>>()
                    .into()),
                Err(err) => Err(err.to_string().into()),
            }
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
            Some(name) => Ok(name.name().to_owned().into()),
            None => Err("Schema is not a named schema".into()),
        }
    }

    /// Returns the namespace of a named schema, or an empty string if the
    /// schema has no namespace. Returns an error for unnamed schemas.
    /// (Named `namespace_name` because `namespace` is a C++ keyword.)
    pub fn namespace_name(&self) -> Result<VecU8, VecU8> {
        match self.schema.name() {
            Some(name) => Ok(name.namespace().unwrap_or_default().into()),
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

    /// Returns the schema variant without allocating a string.
    pub fn schema_type(&self) -> SchemaType {
        match &self.schema {
            Schema::Null => SchemaType::Null,
            Schema::Boolean => SchemaType::Boolean,
            Schema::Int => SchemaType::Int,
            Schema::Long => SchemaType::Long,
            Schema::Float => SchemaType::Float,
            Schema::Double => SchemaType::Double,
            Schema::Bytes => SchemaType::Bytes,
            Schema::String => SchemaType::String,
            Schema::Record(_) => SchemaType::Record,
            Schema::Enum(_) => SchemaType::Enum,
            Schema::Array(_) => SchemaType::Array,
            Schema::Map(_) => SchemaType::Map,
            Schema::Union(_) => SchemaType::Union,
            Schema::Fixed(_) => SchemaType::Fixed,
            Schema::Decimal(_) => SchemaType::Decimal,
            Schema::BigDecimal => SchemaType::BigDecimal,
            Schema::Uuid(_) => SchemaType::Uuid,
            Schema::Date => SchemaType::Date,
            Schema::TimeMillis => SchemaType::TimeMillis,
            Schema::TimeMicros => SchemaType::TimeMicros,
            Schema::TimestampMillis => SchemaType::TimestampMillis,
            Schema::TimestampMicros => SchemaType::TimestampMicros,
            Schema::TimestampNanos => SchemaType::TimestampNanos,
            Schema::LocalTimestampMillis => SchemaType::LocalTimestampMillis,
            Schema::LocalTimestampMicros => SchemaType::LocalTimestampMicros,
            Schema::LocalTimestampNanos => SchemaType::LocalTimestampNanos,
            Schema::Duration(_) => SchemaType::Duration,
            Schema::Ref { .. } => SchemaType::Ref,
        }
    }

    /// Checks that data written with the `writer` schema can be read using
    /// this schema (schema evolution compatibility). Returns an error with
    /// the incompatibility reason otherwise.
    pub fn can_read_from(&self, writer: &AvroSchema) -> Status {
        match SchemaCompatibility::can_read(&writer.schema, &self.schema) {
            Ok(_) => Ok(0),
            Err(err) => Err(err.to_string().into()),
        }
    }

    /// Checks that data written with either schema can be read with the
    /// other one.
    pub fn mutual_read(&self, other: &AvroSchema) -> Status {
        match SchemaCompatibility::mutual_read(&other.schema, &self.schema) {
            Ok(_) => Ok(0),
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
        assert_eq!(schema.schema_type(), SchemaType::Int);
    }

    #[test]
    fn parse_record() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        assert_eq!(schema.schema_type(), SchemaType::Record);
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
        let schema = AvroSchema::parse(br#"{"fields":[], "type":"record", "name":"foo"}"#).unwrap();
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
