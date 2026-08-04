//! Single-datum binary encode/decode, without the object container file
//! framing. Replaces avrocpp's raw binary `Encoder`/`Decoder` usage.

use crate::schema::AvroSchema;
use crate::value::AvroValue;
use crate::vec_u8::{Status, VecU8, catch_panic};
use apache_avro::Schema;
use apache_avro::reader::datum::OwnedGenericDatumReader;

/// Error returned when a single-datum decode leaves bytes unconsumed.
/// A correctly framed datum buffer is fully consumed; leftover bytes
/// signal a framing error that would otherwise be silently dropped.
const TRAILING_BYTES_ERROR: &str = "trailing bytes after single Avro datum";

/// Encodes a value to Avro binary format using the given schema.
pub fn encode_datum(schema: &AvroSchema, value: &AvroValue) -> Result<VecU8, VecU8> {
    catch_panic(
        || match apache_avro::to_avro_datum(&schema.schema, value.value.clone()) {
            Ok(bytes) => Ok(bytes.into()),
            Err(err) => Err(err.to_string().into()),
        },
    )
}

/// Encodes a value using a schema that may reference named types defined in
/// `schemata`.
pub fn encode_datum_schemata(
    schema: &AvroSchema,
    schemata: &[AvroSchema],
    value: &AvroValue,
) -> Result<VecU8, VecU8> {
    catch_panic(|| {
        let schema_refs: Vec<&Schema> = schemata.iter().map(|s| &s.schema).collect();
        match apache_avro::to_avro_datum_schemata(&schema.schema, schema_refs, value.value.clone())
        {
            Ok(bytes) => Ok(bytes.into()),
            Err(err) => Err(err.to_string().into()),
        }
    })
}

/// Decodes a single datum from Avro binary format using the schema it was
/// written with. Errors if the buffer contains trailing bytes after the
/// datum.
pub fn decode_datum(writer_schema: &AvroSchema, data: &[u8]) -> Result<AvroValue, VecU8> {
    catch_panic(|| {
        let mut reader = data;
        let value = apache_avro::from_avro_datum(&writer_schema.schema, &mut reader, None)
            .map_err(|err| VecU8::from(err.to_string()))?;
        if !reader.is_empty() {
            return Err(TRAILING_BYTES_ERROR.into());
        }
        Ok(AvroValue { value })
    })
}

/// Decodes a single datum written with `writer_schema`, resolving it to
/// `reader_schema` (schema evolution). Errors on trailing bytes.
pub fn decode_datum_resolved(
    writer_schema: &AvroSchema,
    reader_schema: &AvroSchema,
    data: &[u8],
) -> Result<AvroValue, VecU8> {
    catch_panic(|| {
        let mut reader = data;
        let value = apache_avro::from_avro_datum(
            &writer_schema.schema,
            &mut reader,
            Some(&reader_schema.schema),
        )
        .map_err(|err| VecU8::from(err.to_string()))?;
        if !reader.is_empty() {
            return Err(TRAILING_BYTES_ERROR.into());
        }
        Ok(AvroValue { value })
    })
}

/// Decodes a single datum whose writer schema may reference named types
/// defined in `writer_schemata`. Errors on trailing bytes.
pub fn decode_datum_schemata(
    writer_schema: &AvroSchema,
    writer_schemata: &[AvroSchema],
    data: &[u8],
) -> Result<AvroValue, VecU8> {
    catch_panic(|| {
        let schema_refs: Vec<&Schema> = writer_schemata.iter().map(|s| &s.schema).collect();
        let mut reader = data;
        let value = apache_avro::from_avro_datum_schemata(
            &writer_schema.schema,
            schema_refs,
            &mut reader,
            None,
        )
        .map_err(|err| VecU8::from(err.to_string()))?;
        if !reader.is_empty() {
            return Err(TRAILING_BYTES_ERROR.into());
        }
        Ok(AvroValue { value })
    })
}

/// Error returned once a reader has been moved from on the C++ side.
const MOVED_OUT_ERROR: &str = "AvroDatumReader is in an invalid, moved-out state";

/// Decodes many datums that share one writer schema.
///
/// The `decode_datum*` functions above rebuild the writer schema's name
/// resolution on every call, which is a fixed per-datum cost that dominates
/// small records. This holds that resolution, so it is paid once at
/// construction, and it can decode into caller-owned storage. See
/// doc/specs/AvroDatumReader.md.
pub struct AvroDatumReader {
    /// `None` only in the moved-from state Crubit leaves behind; see `Default`.
    reader: Option<OwnedGenericDatumReader>,
}

impl Default for AvroDatumReader {
    fn default() -> Self {
        // C++ moves replace the moved-from object with Default::default().
        // Leave it unusable so a use-after-move errors loudly instead of
        // behaving like a reader for some unrelated schema.
        AvroDatumReader { reader: None }
    }
}

impl AvroDatumReader {
    /// Resolves `writer_schema`'s named types once and holds the result.
    /// (Named `create` rather than `new` because `new` is a reserved word in
    /// the generated C++.)
    pub fn create(writer_schema: &AvroSchema) -> Result<AvroDatumReader, VecU8> {
        catch_panic(|| {
            let reader = OwnedGenericDatumReader::new(writer_schema.schema.clone())
                .map_err(|err| VecU8::from(err.to_string()))?;
            Ok(AvroDatumReader {
                reader: Some(reader),
            })
        })
    }

    fn resolved(&self) -> Result<&OwnedGenericDatumReader, VecU8> {
        self.reader.as_ref().ok_or_else(|| MOVED_OUT_ERROR.into())
    }

    /// Decodes one datum into a fresh value. Errors on trailing bytes.
    pub fn decode(&self, data: &[u8]) -> Result<AvroValue, VecU8> {
        let reader = self.resolved()?;
        catch_panic(|| {
            let mut input = data;
            let value = reader
                .read_value(&mut input)
                .map_err(|err| VecU8::from(err.to_string()))?;
            if !input.is_empty() {
                return Err(TRAILING_BYTES_ERROR.into());
            }
            Ok(AvroValue { value })
        })
    }

    /// Decodes one datum into caller-owned storage, reusing compatible
    /// allocations. Errors on trailing bytes, in which case `value` holds the
    /// decoded datum and only the framing is rejected.
    pub fn decode_into(&self, data: &[u8], value: &mut AvroValue) -> Status {
        let reader = self.resolved()?;
        catch_panic(|| {
            let mut input = data;
            reader
                .read_value_into(&mut input, &mut value.value)
                .map_err(|err| VecU8::from(err.to_string()))?;
            if !input.is_empty() {
                return Err(TRAILING_BYTES_ERROR.into());
            }
            Ok(0u8)
        })
    }

    /// Returns the schema this reader decodes with.
    pub fn writer_schema(&self) -> Result<AvroSchema, VecU8> {
        Ok(AvroSchema {
            schema: self.resolved()?.writer_schema().clone(),
        })
    }
}

/// Sets the maximum number of bytes a single decode is allowed to allocate,
/// to bound memory usage on untrusted input.
///
/// This is a process-global that can only be set once, and must be set
/// before the first decode OR container-file read (the container reader
/// also consults it while reading blocks). Later calls have no effect and
/// return the value already in effect. Call this at process startup,
/// before any other Avro operation, or the default (512 MiB) is locked in.
///
/// Note: this bounds value/length-prefix allocations only. It does NOT
/// bound decompression of compressed container-file codecs (deflate,
/// snappy, zstd, bzip2, xz), which can still expand unbounded.
pub fn set_max_allocation_bytes(num_bytes: usize) -> usize {
    apache_avro::util::max_allocation_bytes(num_bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    const RECORD_SCHEMA: &str = r#"{
        "type": "record",
        "name": "Point",
        "fields": [
            {"name": "x", "type": "long"},
            {"name": "y", "type": "long"}
        ]
    }"#;

    fn point(x: i64, y: i64) -> AvroValue {
        let mut record = AvroValue::create_record();
        record.record_put(b"x", &AvroValue::create_long(x)).unwrap();
        record.record_put(b"y", &AvroValue::create_long(y)).unwrap();
        record
    }

    #[test]
    fn datum_roundtrip() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let value = point(3, -4);
        let encoded = encode_datum(&schema, &value).unwrap();
        let decoded = decode_datum(&schema, encoded.as_slice()).unwrap();
        assert!(decoded.equals(&value));
    }

    #[test]
    fn encode_rejects_mismatched_value() {
        let schema = AvroSchema::parse(b"\"string\"").unwrap();
        assert!(encode_datum(&schema, &AvroValue::create_long(1)).is_err());
    }

    #[test]
    fn decode_truncated_datum_fails() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let encoded = encode_datum(&schema, &point(1, 2)).unwrap();
        let truncated = &encoded.as_slice()[..encoded.len() - 1];
        assert!(decode_datum(&schema, truncated).is_err());
    }

    #[test]
    fn decode_rejects_trailing_bytes() {
        let schema = AvroSchema::parse(b"\"int\"").unwrap();
        let mut encoded = encode_datum(&schema, &AvroValue::create_int(1))
            .unwrap()
            .into_vec();
        // A valid single datum followed by stray bytes must be rejected
        // rather than silently decoding only the first datum.
        encoded.extend_from_slice(&[0xff, 0xfe]);
        let err = decode_datum(&schema, &encoded).unwrap_err();
        let message = String::from_utf8(err.into_vec()).unwrap();
        assert!(
            message.contains("trailing bytes"),
            "unexpected error: {message}"
        );
    }

    #[test]
    fn decode_resolved_promotes_schema() {
        // Writer writes int, reader resolves to long.
        let writer_schema = AvroSchema::parse(b"\"int\"").unwrap();
        let reader_schema = AvroSchema::parse(b"\"long\"").unwrap();
        let encoded = encode_datum(&writer_schema, &AvroValue::create_int(99)).unwrap();
        let decoded =
            decode_datum_resolved(&writer_schema, &reader_schema, encoded.as_slice()).unwrap();
        assert_eq!(decoded.get_long().unwrap(), 99);
    }

    #[test]
    fn decode_resolved_with_added_field_default() {
        let writer = AvroSchema::parse(
            br#"{"type": "record", "name": "R", "fields": [
                {"name": "a", "type": "long"}]}"#,
        )
        .unwrap();
        let reader = AvroSchema::parse(
            br#"{"type": "record", "name": "R", "fields": [
                {"name": "a", "type": "long"},
                {"name": "b", "type": "string", "default": "fallback"}]}"#,
        )
        .unwrap();
        let mut record = AvroValue::create_record();
        record.record_put(b"a", &AvroValue::create_long(5)).unwrap();
        let encoded = encode_datum(&writer, &record).unwrap();
        let decoded = decode_datum_resolved(&writer, &reader, encoded.as_slice()).unwrap();
        assert_eq!(
            decoded.get_record_field(b"a").unwrap().get_long().unwrap(),
            5
        );
        assert_eq!(
            decoded
                .get_record_field(b"b")
                .unwrap()
                .get_string()
                .unwrap()
                .as_slice(),
            b"fallback"
        );
    }

    #[test]
    fn max_allocation_bytes_first_call_wins() {
        // The limit is process-global and can only be initialized once
        // (other tests' decodes may already have initialized it to the
        // default), so assert the invariant rather than a specific value:
        // a second call with a different argument returns whatever the
        // first observed call returned.
        let first = set_max_allocation_bytes(512 * 1024 * 1024);
        let second = set_max_allocation_bytes(1024);
        assert_eq!(first, second);
    }

    #[test]
    fn schemata_roundtrip_with_cross_reference() {
        let address_json = VecU8::from(
            r#"{"type": "record", "name": "Address", "fields": [
                {"name": "city", "type": "string"}]}"#,
        );
        let person_json = VecU8::from(
            r#"{"type": "record", "name": "Person", "fields": [
                {"name": "address", "type": "Address"}]}"#,
        );
        let schemas = AvroSchema::parse_list(&[address_json, person_json]).unwrap();
        let schemas = schemas.as_slice();
        let person_schema = &schemas[1];

        let mut address = AvroValue::create_record();
        address
            .record_put(b"city", &AvroValue::create_string(b"Zurich").unwrap())
            .unwrap();
        let mut person = AvroValue::create_record();
        person.record_put(b"address", &address).unwrap();

        let encoded = encode_datum_schemata(person_schema, schemas, &person).unwrap();
        let decoded = decode_datum_schemata(person_schema, schemas, encoded.as_slice()).unwrap();
        assert!(decoded.equals(&person));
    }

    #[test]
    fn reader_decodes_same_values_as_the_free_function() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let reader = AvroDatumReader::create(&schema).unwrap();
        for (x, y) in [(0, 0), (3, -4), (i64::MAX, i64::MIN)] {
            let encoded = encode_datum(&schema, &point(x, y)).unwrap();
            let by_function = decode_datum(&schema, encoded.as_slice()).unwrap();
            let by_reader = reader.decode(encoded.as_slice()).unwrap();
            assert!(by_reader.equals(&by_function));
        }
    }

    #[test]
    fn reader_decode_into_reuses_storage() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let reader = AvroDatumReader::create(&schema).unwrap();
        let first = encode_datum(&schema, &point(1, 2)).unwrap();
        let second = encode_datum(&schema, &point(7, 8)).unwrap();

        let mut value = AvroValue::default();
        reader.decode_into(first.as_slice(), &mut value).unwrap();
        let record_ptr = match &value.value {
            apache_avro::types::Value::Record(fields) => fields.as_ptr(),
            other => panic!("expected a record, got {other:?}"),
        };

        reader.decode_into(second.as_slice(), &mut value).unwrap();
        match &value.value {
            apache_avro::types::Value::Record(fields) => {
                assert_eq!(record_ptr, fields.as_ptr(), "record vector was reallocated");
            }
            other => panic!("expected a record, got {other:?}"),
        }
        assert_eq!(value.get_record_field(b"x").unwrap().get_long().unwrap(), 7);
    }

    #[test]
    fn reader_decode_into_overwrites_a_differently_shaped_value() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let reader = AvroDatumReader::create(&schema).unwrap();
        let encoded = encode_datum(&schema, &point(5, 6)).unwrap();

        // Storage holding an unrelated shape must be replaced, not merged.
        let mut value = AvroValue::create_string(b"not a record").unwrap();
        reader.decode_into(encoded.as_slice(), &mut value).unwrap();
        assert_eq!(value.get_record_field(b"y").unwrap().get_long().unwrap(), 6);
    }

    #[test]
    fn reader_rejects_trailing_bytes() {
        let schema = AvroSchema::parse(b"\"int\"").unwrap();
        let reader = AvroDatumReader::create(&schema).unwrap();
        let mut encoded = encode_datum(&schema, &AvroValue::create_int(1))
            .unwrap()
            .into_vec();
        encoded.extend_from_slice(&[0xff, 0xfe]);

        for message in [
            String::from_utf8(reader.decode(&encoded).unwrap_err().into_vec()).unwrap(),
            String::from_utf8(
                reader
                    .decode_into(&encoded, &mut AvroValue::default())
                    .unwrap_err()
                    .into_vec(),
            )
            .unwrap(),
        ] {
            assert!(
                message.contains("trailing bytes"),
                "unexpected error: {message}"
            );
        }
    }

    #[test]
    fn reader_rejects_truncated_datum() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let reader = AvroDatumReader::create(&schema).unwrap();
        let encoded = encode_datum(&schema, &point(1, 2)).unwrap();
        let truncated = &encoded.as_slice()[..encoded.len() - 1];
        assert!(reader.decode(truncated).is_err());
    }

    #[test]
    fn reader_resolves_named_references_within_one_schema() {
        // A self-contained recursive schema: the reader must resolve `Node`
        // for the nested reference, which is what ResolvedOwnedSchema buys.
        let schema = AvroSchema::parse(
            br#"{"type": "record", "name": "Node", "fields": [
                {"name": "value", "type": "long"},
                {"name": "next", "type": ["null", "Node"]}]}"#,
        )
        .unwrap();
        let reader = AvroDatumReader::create(&schema).unwrap();

        let mut leaf = AvroValue::create_record();
        leaf.record_put(b"value", &AvroValue::create_long(2))
            .unwrap();
        leaf.record_put(b"next", &AvroValue::create_null()).unwrap();
        let mut root = AvroValue::create_record();
        root.record_put(b"value", &AvroValue::create_long(1))
            .unwrap();
        root.record_put(b"next", &leaf).unwrap();

        let encoded = encode_datum(&schema, &root).unwrap();
        let by_reader = reader.decode(encoded.as_slice()).unwrap();
        // Compared against the free function rather than against `root`:
        // writing a union field accepts a bare branch value, but decoding
        // always produces the Union wrapper, so `root` is not the decoded
        // shape. What matters here is that both decoders agree.
        let by_function = decode_datum(&schema, encoded.as_slice()).unwrap();
        assert!(by_reader.equals(&by_function));
        // Decoding at all proves the `Node` self-reference resolved; an
        // unresolved name fails the decode outright. Step through the union
        // explicitly, since only AvroPath treats unions as transparent.
        assert_eq!(
            by_reader
                .get_record_field(b"next")
                .unwrap()
                .get_union_value()
                .unwrap()
                .get_record_field(b"value")
                .unwrap()
                .get_long()
                .unwrap(),
            2
        );
    }

    #[test]
    fn moved_out_reader_errors_instead_of_decoding() {
        let schema = AvroSchema::parse(b"\"int\"").unwrap();
        let encoded = encode_datum(&schema, &AvroValue::create_int(1)).unwrap();
        let moved_out = AvroDatumReader::default();

        assert!(moved_out.decode(encoded.as_slice()).is_err());
        assert!(
            moved_out
                .decode_into(encoded.as_slice(), &mut AvroValue::default())
                .is_err()
        );
        assert!(moved_out.writer_schema().is_err());
    }

    #[test]
    fn reader_reports_its_writer_schema() {
        let schema = AvroSchema::parse(RECORD_SCHEMA.as_bytes()).unwrap();
        let reader = AvroDatumReader::create(&schema).unwrap();
        assert!(reader.writer_schema().unwrap() == schema);
    }
}
