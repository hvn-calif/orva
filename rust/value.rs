//! Crubit-friendly wrapper around `apache_avro::types::Value`.
//! Replaces avrocpp's `GenericDatum`.

use crate::make_vec_type;
use crate::schema::AvroSchema;
use crate::vec_u8::{catch_panic, utf8, Status, VecU8};
use apache_avro::types::Value;
use apache_avro::{Days, Decimal, Duration, Millis, Months};

make_vec_type!(VecU8, VecVecU8);

/// A generic Avro value tree. Replaces avrocpp's `GenericDatum`.
///
/// Values are built with the `create_*` constructors and `record_put` /
/// `array_push` / `map_put` mutators, inspected with the `is_*` predicates
/// and read with the `get_*` accessors. Accessors return clones; the
/// boundary cannot return references.
#[derive(Debug, Clone, PartialEq)]
pub struct AvroValue {
    pub(crate) value: Value,
}

impl Default for AvroValue {
    fn default() -> Self {
        AvroValue { value: Value::Null }
    }
}

impl AvroValue {
    fn wrap(value: Value) -> AvroValue {
        AvroValue { value }
    }

    // ------------------------------------------------------------------
    // Constructors: primitive types.
    // ------------------------------------------------------------------

    pub fn create_null() -> AvroValue {
        Self::wrap(Value::Null)
    }

    pub fn create_boolean(v: bool) -> AvroValue {
        Self::wrap(Value::Boolean(v))
    }

    pub fn create_int(v: i32) -> AvroValue {
        Self::wrap(Value::Int(v))
    }

    pub fn create_long(v: i64) -> AvroValue {
        Self::wrap(Value::Long(v))
    }

    pub fn create_float(v: f32) -> AvroValue {
        Self::wrap(Value::Float(v))
    }

    pub fn create_double(v: f64) -> AvroValue {
        Self::wrap(Value::Double(v))
    }

    pub fn create_bytes(v: &[u8]) -> AvroValue {
        Self::wrap(Value::Bytes(v.to_vec()))
    }

    /// Creates a string value. Returns an error if `v` is not valid UTF-8.
    pub fn create_string(v: &[u8]) -> Result<AvroValue, VecU8> {
        Ok(Self::wrap(Value::String(utf8(v)?.to_string())))
    }

    // ------------------------------------------------------------------
    // Constructors: complex types.
    // ------------------------------------------------------------------

    /// Creates an empty record. Add fields with `record_put`.
    pub fn create_record() -> AvroValue {
        Self::wrap(Value::Record(Vec::new()))
    }

    /// Creates an empty array. Add items with `array_push`.
    pub fn create_array() -> AvroValue {
        Self::wrap(Value::Array(Vec::new()))
    }

    /// Creates an empty map. Add entries with `map_put`.
    pub fn create_map() -> AvroValue {
        Self::wrap(Value::Map(Default::default()))
    }

    /// Creates an enum value from the symbol's position in the schema and
    /// the symbol name. Returns an error if `symbol` is not valid UTF-8.
    pub fn create_enum(position: u32, symbol: &[u8]) -> Result<AvroValue, VecU8> {
        Ok(Self::wrap(Value::Enum(position, utf8(symbol)?.to_string())))
    }

    pub fn create_fixed(v: &[u8]) -> AvroValue {
        Self::wrap(Value::Fixed(v.len(), v.to_vec()))
    }

    /// Creates a union value holding `v` at the given zero-based branch
    /// index of the union schema.
    pub fn create_union(branch_index: u32, v: &AvroValue) -> AvroValue {
        Self::wrap(Value::Union(branch_index, Box::new(v.value.clone())))
    }

    // ------------------------------------------------------------------
    // Constructors: logical types.
    // ------------------------------------------------------------------

    /// Creates a decimal value from its two's-complement big-endian
    /// representation (as serialized by Avro).
    pub fn create_decimal(v: &[u8]) -> AvroValue {
        Self::wrap(Value::Decimal(Decimal::from(v)))
    }

    /// Creates a UUID value from its string representation.
    pub fn create_uuid(v: &[u8]) -> Result<AvroValue, VecU8> {
        let uuid = uuid::Uuid::parse_str(utf8(v)?).map_err(|err| VecU8::from(err.to_string()))?;
        Ok(Self::wrap(Value::Uuid(uuid)))
    }

    /// Creates a date value (days since the Unix epoch).
    pub fn create_date(days: i32) -> AvroValue {
        Self::wrap(Value::Date(days))
    }

    pub fn create_time_millis(v: i32) -> AvroValue {
        Self::wrap(Value::TimeMillis(v))
    }

    pub fn create_time_micros(v: i64) -> AvroValue {
        Self::wrap(Value::TimeMicros(v))
    }

    pub fn create_timestamp_millis(v: i64) -> AvroValue {
        Self::wrap(Value::TimestampMillis(v))
    }

    pub fn create_timestamp_micros(v: i64) -> AvroValue {
        Self::wrap(Value::TimestampMicros(v))
    }

    pub fn create_timestamp_nanos(v: i64) -> AvroValue {
        Self::wrap(Value::TimestampNanos(v))
    }

    pub fn create_local_timestamp_millis(v: i64) -> AvroValue {
        Self::wrap(Value::LocalTimestampMillis(v))
    }

    pub fn create_local_timestamp_micros(v: i64) -> AvroValue {
        Self::wrap(Value::LocalTimestampMicros(v))
    }

    pub fn create_local_timestamp_nanos(v: i64) -> AvroValue {
        Self::wrap(Value::LocalTimestampNanos(v))
    }

    pub fn create_duration(months: u32, days: u32, millis: u32) -> AvroValue {
        Self::wrap(Value::Duration(Duration::new(
            Months::new(months),
            Days::new(days),
            Millis::new(millis),
        )))
    }

    // ------------------------------------------------------------------
    // Accessors. Each returns an error if the value has a different type.
    // ------------------------------------------------------------------

    pub fn get_boolean(&self) -> Result<bool, VecU8> {
        match &self.value {
            Value::Boolean(v) => Ok(*v),
            _ => Err(self.type_error("boolean")),
        }
    }

    pub fn get_int(&self) -> Result<i32, VecU8> {
        match &self.value {
            Value::Int(v) => Ok(*v),
            _ => Err(self.type_error("int")),
        }
    }

    pub fn get_long(&self) -> Result<i64, VecU8> {
        match &self.value {
            Value::Long(v) => Ok(*v),
            _ => Err(self.type_error("long")),
        }
    }

    pub fn get_float(&self) -> Result<f32, VecU8> {
        match &self.value {
            Value::Float(v) => Ok(*v),
            _ => Err(self.type_error("float")),
        }
    }

    pub fn get_double(&self) -> Result<f64, VecU8> {
        match &self.value {
            Value::Double(v) => Ok(*v),
            _ => Err(self.type_error("double")),
        }
    }

    pub fn get_bytes(&self) -> Result<VecU8, VecU8> {
        match &self.value {
            Value::Bytes(v) => Ok(v.as_slice().into()),
            _ => Err(self.type_error("bytes")),
        }
    }

    pub fn get_string(&self) -> Result<VecU8, VecU8> {
        match &self.value {
            Value::String(v) => Ok(v.as_str().into()),
            _ => Err(self.type_error("string")),
        }
    }

    pub fn get_enum_position(&self) -> Result<u32, VecU8> {
        match &self.value {
            Value::Enum(position, _) => Ok(*position),
            _ => Err(self.type_error("enum")),
        }
    }

    pub fn get_enum_symbol(&self) -> Result<VecU8, VecU8> {
        match &self.value {
            Value::Enum(_, symbol) => Ok(symbol.as_str().into()),
            _ => Err(self.type_error("enum")),
        }
    }

    pub fn get_fixed_bytes(&self) -> Result<VecU8, VecU8> {
        match &self.value {
            Value::Fixed(_, v) => Ok(v.as_slice().into()),
            _ => Err(self.type_error("fixed")),
        }
    }

    /// Returns the zero-based branch index of a union value.
    pub fn get_union_branch(&self) -> Result<u32, VecU8> {
        match &self.value {
            Value::Union(branch_index, _) => Ok(*branch_index),
            _ => Err(self.type_error("union")),
        }
    }

    /// Returns the value held by a union.
    pub fn get_union_value(&self) -> Result<AvroValue, VecU8> {
        match &self.value {
            Value::Union(_, v) => Ok(Self::wrap(v.as_ref().clone())),
            _ => Err(self.type_error("union")),
        }
    }

    // ------------------------------------------------------------------
    // Accessors: logical types.
    // ------------------------------------------------------------------

    /// Returns the two's-complement big-endian representation of a decimal
    /// value.
    pub fn get_decimal_bytes(&self) -> Result<VecU8, VecU8> {
        match &self.value {
            Value::Decimal(v) => match Vec::<u8>::try_from(v) {
                Ok(bytes) => Ok(bytes.into()),
                Err(err) => Err(err.to_string().into()),
            },
            _ => Err(self.type_error("decimal")),
        }
    }

    /// Returns the hyphenated string representation of a UUID value.
    pub fn get_uuid(&self) -> Result<VecU8, VecU8> {
        match &self.value {
            Value::Uuid(v) => Ok(v.to_string().into()),
            _ => Err(self.type_error("uuid")),
        }
    }

    pub fn get_date(&self) -> Result<i32, VecU8> {
        match &self.value {
            Value::Date(v) => Ok(*v),
            _ => Err(self.type_error("date")),
        }
    }

    pub fn get_time_millis(&self) -> Result<i32, VecU8> {
        match &self.value {
            Value::TimeMillis(v) => Ok(*v),
            _ => Err(self.type_error("time-millis")),
        }
    }

    pub fn get_time_micros(&self) -> Result<i64, VecU8> {
        match &self.value {
            Value::TimeMicros(v) => Ok(*v),
            _ => Err(self.type_error("time-micros")),
        }
    }

    pub fn get_timestamp_millis(&self) -> Result<i64, VecU8> {
        match &self.value {
            Value::TimestampMillis(v) => Ok(*v),
            _ => Err(self.type_error("timestamp-millis")),
        }
    }

    pub fn get_timestamp_micros(&self) -> Result<i64, VecU8> {
        match &self.value {
            Value::TimestampMicros(v) => Ok(*v),
            _ => Err(self.type_error("timestamp-micros")),
        }
    }

    pub fn get_timestamp_nanos(&self) -> Result<i64, VecU8> {
        match &self.value {
            Value::TimestampNanos(v) => Ok(*v),
            _ => Err(self.type_error("timestamp-nanos")),
        }
    }

    pub fn get_local_timestamp_millis(&self) -> Result<i64, VecU8> {
        match &self.value {
            Value::LocalTimestampMillis(v) => Ok(*v),
            _ => Err(self.type_error("local-timestamp-millis")),
        }
    }

    pub fn get_local_timestamp_micros(&self) -> Result<i64, VecU8> {
        match &self.value {
            Value::LocalTimestampMicros(v) => Ok(*v),
            _ => Err(self.type_error("local-timestamp-micros")),
        }
    }

    pub fn get_local_timestamp_nanos(&self) -> Result<i64, VecU8> {
        match &self.value {
            Value::LocalTimestampNanos(v) => Ok(*v),
            _ => Err(self.type_error("local-timestamp-nanos")),
        }
    }

    pub fn get_duration_months(&self) -> Result<u32, VecU8> {
        match &self.value {
            Value::Duration(v) => Ok(v.months().into()),
            _ => Err(self.type_error("duration")),
        }
    }

    pub fn get_duration_days(&self) -> Result<u32, VecU8> {
        match &self.value {
            Value::Duration(v) => Ok(v.days().into()),
            _ => Err(self.type_error("duration")),
        }
    }

    pub fn get_duration_millis(&self) -> Result<u32, VecU8> {
        match &self.value {
            Value::Duration(v) => Ok(v.millis().into()),
            _ => Err(self.type_error("duration")),
        }
    }

    // ------------------------------------------------------------------
    // Accessors: arrays, maps and records.
    // ------------------------------------------------------------------

    /// (Returns u64 because Crubit cannot bridge `Result<usize, _>`.)
    pub fn get_array_len(&self) -> Result<u64, VecU8> {
        match &self.value {
            Value::Array(items) => Ok(items.len() as u64),
            _ => Err(self.type_error("array")),
        }
    }

    pub fn get_array_item(&self, index: usize) -> Result<AvroValue, VecU8> {
        match &self.value {
            Value::Array(items) => match items.get(index) {
                Some(item) => Ok(Self::wrap(item.clone())),
                None => Err(format!(
                    "Array index {} out of bounds (len {})",
                    index,
                    items.len()
                )
                .into()),
            },
            _ => Err(self.type_error("array")),
        }
    }

    /// (Returns u64 because Crubit cannot bridge `Result<usize, _>`.)
    pub fn get_map_len(&self) -> Result<u64, VecU8> {
        match &self.value {
            Value::Map(entries) => Ok(entries.len() as u64),
            _ => Err(self.type_error("map")),
        }
    }

    /// Returns the keys of a map, sorted lexicographically for determinism
    /// (the underlying map is unordered).
    pub fn get_map_keys(&self) -> Result<VecVecU8, VecU8> {
        match &self.value {
            Value::Map(entries) => {
                let mut keys: Vec<&String> = entries.keys().collect();
                keys.sort();
                Ok(keys.into_iter().map(|k| k.as_str().into()).collect::<Vec<VecU8>>().into())
            }
            _ => Err(self.type_error("map")),
        }
    }

    pub fn get_map_value(&self, raw_key: &[u8]) -> Result<AvroValue, VecU8> {
        let key = utf8(raw_key)?;
        match &self.value {
            Value::Map(entries) => match entries.get(key) {
                Some(v) => Ok(Self::wrap(v.clone())),
                None => Err(format!("Key '{}' not found in map", key).into()),
            },
            _ => Err(self.type_error("map")),
        }
    }

    pub fn has_map_key(&self, raw_key: &[u8]) -> Result<bool, VecU8> {
        let key = utf8(raw_key)?;
        match &self.value {
            Value::Map(entries) => Ok(entries.contains_key(key)),
            _ => Err(self.type_error("map")),
        }
    }

    /// (Returns u64 because Crubit cannot bridge `Result<usize, _>`.)
    pub fn get_record_len(&self) -> Result<u64, VecU8> {
        match &self.value {
            Value::Record(fields) => Ok(fields.len() as u64),
            _ => Err(self.type_error("record")),
        }
    }

    /// Returns the field names of a record, in field order.
    pub fn get_record_field_names(&self) -> Result<VecVecU8, VecU8> {
        match &self.value {
            Value::Record(fields) => Ok(fields
                .iter()
                .map(|(name, _)| name.as_str().into())
                .collect::<Vec<VecU8>>()
                .into()),
            _ => Err(self.type_error("record")),
        }
    }

    pub fn get_record_field(&self, raw_name: &[u8]) -> Result<AvroValue, VecU8> {
        let name = utf8(raw_name)?;
        match &self.value {
            Value::Record(fields) => {
                match fields.iter().find(|(field_name, _)| field_name == name) {
                    Some((_, v)) => Ok(Self::wrap(v.clone())),
                    None => Err(format!("Field '{}' not found in record", name).into()),
                }
            }
            _ => Err(self.type_error("record")),
        }
    }

    pub fn has_record_field(&self, raw_name: &[u8]) -> Result<bool, VecU8> {
        let name = utf8(raw_name)?;
        match &self.value {
            Value::Record(fields) => {
                Ok(fields.iter().any(|(field_name, _)| field_name == name))
            }
            _ => Err(self.type_error("record")),
        }
    }

    // ------------------------------------------------------------------
    // Mutators.
    // ------------------------------------------------------------------

    /// Sets a record field, replacing the value if the field already exists.
    pub fn record_put(&mut self, raw_name: &[u8], v: &AvroValue) -> Status {
        let name = utf8(raw_name)?;
        match &mut self.value {
            Value::Record(fields) => {
                match fields.iter_mut().find(|(field_name, _)| field_name == name) {
                    Some((_, existing)) => *existing = v.value.clone(),
                    None => fields.push((name.to_string(), v.value.clone())),
                }
                Ok(0)
            }
            _ => Err(self.type_error("record")),
        }
    }

    /// Appends an item to an array value.
    pub fn array_push(&mut self, v: &AvroValue) -> Status {
        match &mut self.value {
            Value::Array(items) => {
                items.push(v.value.clone());
                Ok(0)
            }
            _ => Err(self.type_error("array")),
        }
    }

    /// Inserts an entry into a map value, replacing any existing entry with
    /// the same key.
    pub fn map_put(&mut self, raw_key: &[u8], v: &AvroValue) -> Status {
        let key = utf8(raw_key)?;
        match &mut self.value {
            Value::Map(entries) => {
                entries.insert(key.to_string(), v.value.clone());
                Ok(0)
            }
            _ => Err(self.type_error("map")),
        }
    }

    // ------------------------------------------------------------------
    // Type predicates.
    // ------------------------------------------------------------------

    pub fn is_null(&self) -> bool {
        matches!(self.value, Value::Null)
    }

    pub fn is_boolean(&self) -> bool {
        matches!(self.value, Value::Boolean(_))
    }

    pub fn is_int(&self) -> bool {
        matches!(self.value, Value::Int(_))
    }

    pub fn is_long(&self) -> bool {
        matches!(self.value, Value::Long(_))
    }

    pub fn is_float(&self) -> bool {
        matches!(self.value, Value::Float(_))
    }

    pub fn is_double(&self) -> bool {
        matches!(self.value, Value::Double(_))
    }

    pub fn is_bytes(&self) -> bool {
        matches!(self.value, Value::Bytes(_))
    }

    pub fn is_string(&self) -> bool {
        matches!(self.value, Value::String(_))
    }

    pub fn is_record(&self) -> bool {
        matches!(self.value, Value::Record(_))
    }

    pub fn is_enum(&self) -> bool {
        matches!(self.value, Value::Enum(_, _))
    }

    pub fn is_array(&self) -> bool {
        matches!(self.value, Value::Array(_))
    }

    pub fn is_map(&self) -> bool {
        matches!(self.value, Value::Map(_))
    }

    pub fn is_union(&self) -> bool {
        matches!(self.value, Value::Union(_, _))
    }

    pub fn is_fixed(&self) -> bool {
        matches!(self.value, Value::Fixed(_, _))
    }

    pub fn is_decimal(&self) -> bool {
        matches!(self.value, Value::Decimal(_))
    }

    pub fn is_uuid(&self) -> bool {
        matches!(self.value, Value::Uuid(_))
    }

    pub fn is_date(&self) -> bool {
        matches!(self.value, Value::Date(_))
    }

    pub fn is_time_millis(&self) -> bool {
        matches!(self.value, Value::TimeMillis(_))
    }

    pub fn is_time_micros(&self) -> bool {
        matches!(self.value, Value::TimeMicros(_))
    }

    pub fn is_timestamp_millis(&self) -> bool {
        matches!(self.value, Value::TimestampMillis(_))
    }

    pub fn is_timestamp_micros(&self) -> bool {
        matches!(self.value, Value::TimestampMicros(_))
    }

    pub fn is_timestamp_nanos(&self) -> bool {
        matches!(self.value, Value::TimestampNanos(_))
    }

    pub fn is_local_timestamp_millis(&self) -> bool {
        matches!(self.value, Value::LocalTimestampMillis(_))
    }

    pub fn is_local_timestamp_micros(&self) -> bool {
        matches!(self.value, Value::LocalTimestampMicros(_))
    }

    pub fn is_local_timestamp_nanos(&self) -> bool {
        matches!(self.value, Value::LocalTimestampNanos(_))
    }

    pub fn is_duration(&self) -> bool {
        matches!(self.value, Value::Duration(_))
    }

    /// Returns the name of the value's type, e.g. "record" or "int".
    pub fn type_name(&self) -> VecU8 {
        self.type_name_str().into()
    }

    // ------------------------------------------------------------------
    // Validation, resolution, conversion.
    // ------------------------------------------------------------------

    /// Returns true if this value conforms to the given schema.
    pub fn validate(&self, schema: &AvroSchema) -> bool {
        self.value.validate(&schema.schema)
    }

    /// Performs schema resolution: adapts this value to the given schema
    /// (e.g. promotes int to long, selects union branches), returning the
    /// resolved value. Returns an error if the value cannot be resolved.
    pub fn resolve(&self, schema: &AvroSchema) -> Result<AvroValue, VecU8> {
        catch_panic(|| match self.value.clone().resolve(&schema.schema) {
            Ok(value) => Ok(Self::wrap(value)),
            Err(err) => Err(err.to_string().into()),
        })
    }

    /// Converts this value to a JSON string. Bytes and fixed values are
    /// rendered as arrays of numbers, following apache-avro's conversion.
    pub fn to_json_string(&self) -> Result<VecU8, VecU8> {
        catch_panic(|| {
            let json = serde_json::Value::try_from(self.value.clone())
                .map_err(|err| VecU8::from(err.to_string()))?;
            serde_json::to_string(&json).map(VecU8::from).map_err(|err| err.to_string().into())
        })
    }

    /// Compares two values for equality.
    pub fn equals(&self, other: &AvroValue) -> bool {
        self.value == other.value
    }

    fn type_name_str(&self) -> &'static str {
        match &self.value {
            Value::Null => "null",
            Value::Boolean(_) => "boolean",
            Value::Int(_) => "int",
            Value::Long(_) => "long",
            Value::Float(_) => "float",
            Value::Double(_) => "double",
            Value::Bytes(_) => "bytes",
            Value::String(_) => "string",
            Value::Fixed(_, _) => "fixed",
            Value::Enum(_, _) => "enum",
            Value::Union(_, _) => "union",
            Value::Array(_) => "array",
            Value::Map(_) => "map",
            Value::Record(_) => "record",
            Value::Date(_) => "date",
            Value::Decimal(_) => "decimal",
            Value::BigDecimal(_) => "big-decimal",
            Value::TimeMillis(_) => "time-millis",
            Value::TimeMicros(_) => "time-micros",
            Value::TimestampMillis(_) => "timestamp-millis",
            Value::TimestampMicros(_) => "timestamp-micros",
            Value::TimestampNanos(_) => "timestamp-nanos",
            Value::LocalTimestampMillis(_) => "local-timestamp-millis",
            Value::LocalTimestampMicros(_) => "local-timestamp-micros",
            Value::LocalTimestampNanos(_) => "local-timestamp-nanos",
            Value::Duration(_) => "duration",
            Value::Uuid(_) => "uuid",
        }
    }

    fn type_error(&self, expected: &str) -> VecU8 {
        format!("Value is not {}; actual type is {}", expected, self.type_name_str()).into()
    }
}

make_vec_type!(AvroValue, VecAvroValue);

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn primitive_roundtrips() {
        assert!(AvroValue::create_null().is_null());
        assert!(AvroValue::create_boolean(true).get_boolean().unwrap());
        assert_eq!(AvroValue::create_int(-42).get_int().unwrap(), -42);
        assert_eq!(AvroValue::create_long(1 << 40).get_long().unwrap(), 1 << 40);
        assert_eq!(AvroValue::create_float(1.5).get_float().unwrap(), 1.5);
        assert_eq!(AvroValue::create_double(2.5).get_double().unwrap(), 2.5);
        assert_eq!(
            AvroValue::create_bytes(&[1, 2, 3]).get_bytes().unwrap().as_slice(),
            &[1, 2, 3]
        );
        assert_eq!(
            AvroValue::create_string(b"hello").unwrap().get_string().unwrap().as_slice(),
            b"hello"
        );
    }

    #[test]
    fn default_is_null() {
        assert!(AvroValue::default().is_null());
    }

    #[test]
    fn wrong_type_access_fails_with_message() {
        let v = AvroValue::create_int(1);
        let err = v.get_string().unwrap_err();
        let message = String::from_utf8(err.into_vec()).unwrap();
        assert!(message.contains("not string"));
        assert!(message.contains("int"));
    }

    #[test]
    fn invalid_utf8_string_fails() {
        assert!(AvroValue::create_string(&[0xff, 0xfe]).is_err());
        assert!(AvroValue::create_enum(0, &[0xff]).is_err());
    }

    #[test]
    fn enum_roundtrip() {
        let v = AvroValue::create_enum(2, b"GREEN").unwrap();
        assert!(v.is_enum());
        assert_eq!(v.get_enum_position().unwrap(), 2);
        assert_eq!(v.get_enum_symbol().unwrap().as_slice(), b"GREEN");
    }

    #[test]
    fn fixed_roundtrip() {
        let v = AvroValue::create_fixed(&[9, 8, 7, 6]);
        assert!(v.is_fixed());
        assert_eq!(v.get_fixed_bytes().unwrap().as_slice(), &[9, 8, 7, 6]);
    }

    #[test]
    fn union_roundtrip() {
        let inner = AvroValue::create_long(7);
        let v = AvroValue::create_union(1, &inner);
        assert!(v.is_union());
        assert_eq!(v.get_union_branch().unwrap(), 1);
        assert!(v.get_union_value().unwrap().equals(&inner));
    }

    #[test]
    fn record_field_operations() {
        let mut record = AvroValue::create_record();
        record.record_put(b"a", &AvroValue::create_int(1)).unwrap();
        record.record_put(b"b", &AvroValue::create_int(2)).unwrap();
        assert_eq!(record.get_record_len().unwrap(), 2);
        assert!(record.has_record_field(b"a").unwrap());
        assert!(!record.has_record_field(b"z").unwrap());
        assert_eq!(record.get_record_field(b"b").unwrap().get_int().unwrap(), 2);
        assert!(record.get_record_field(b"z").is_err());

        // Replacing an existing field keeps field order and count.
        record.record_put(b"a", &AvroValue::create_int(10)).unwrap();
        assert_eq!(record.get_record_len().unwrap(), 2);
        assert_eq!(record.get_record_field(b"a").unwrap().get_int().unwrap(), 10);

        let names = record.get_record_field_names().unwrap();
        assert_eq!(names.as_slice()[0].as_slice(), b"a");
        assert_eq!(names.as_slice()[1].as_slice(), b"b");
    }

    #[test]
    fn array_operations() {
        let mut array = AvroValue::create_array();
        array.array_push(&AvroValue::create_int(1)).unwrap();
        array.array_push(&AvroValue::create_int(2)).unwrap();
        assert_eq!(array.get_array_len().unwrap(), 2);
        assert_eq!(array.get_array_item(1).unwrap().get_int().unwrap(), 2);
        assert!(array.get_array_item(2).is_err());
        // Mutating a non-array fails.
        assert!(AvroValue::create_int(0).array_push(&AvroValue::create_null()).is_err());
    }

    #[test]
    fn map_operations() {
        let mut map = AvroValue::create_map();
        map.map_put(b"zebra", &AvroValue::create_int(1)).unwrap();
        map.map_put(b"apple", &AvroValue::create_int(2)).unwrap();
        assert_eq!(map.get_map_len().unwrap(), 2);
        assert!(map.has_map_key(b"apple").unwrap());
        assert!(!map.has_map_key(b"pear").unwrap());
        assert_eq!(map.get_map_value(b"zebra").unwrap().get_int().unwrap(), 1);
        assert!(map.get_map_value(b"pear").is_err());

        // Keys come back sorted.
        let keys = map.get_map_keys().unwrap();
        assert_eq!(keys.as_slice()[0].as_slice(), b"apple");
        assert_eq!(keys.as_slice()[1].as_slice(), b"zebra");

        // Overwriting a key does not grow the map.
        map.map_put(b"apple", &AvroValue::create_int(3)).unwrap();
        assert_eq!(map.get_map_len().unwrap(), 2);
        assert_eq!(map.get_map_value(b"apple").unwrap().get_int().unwrap(), 3);
    }

    #[test]
    fn logical_type_roundtrips() {
        let decimal = AvroValue::create_decimal(&[1, 24]);
        assert!(decimal.is_decimal());
        assert_eq!(decimal.get_decimal_bytes().unwrap().as_slice(), &[1, 24]);

        let uuid = AvroValue::create_uuid(b"6f2b0e76-4d3d-4f8e-9d3a-2e1b8a7c6d5e").unwrap();
        assert!(uuid.is_uuid());
        assert_eq!(
            uuid.get_uuid().unwrap().as_slice(),
            b"6f2b0e76-4d3d-4f8e-9d3a-2e1b8a7c6d5e"
        );
        assert!(AvroValue::create_uuid(b"not-a-uuid").is_err());

        assert_eq!(AvroValue::create_date(19000).get_date().unwrap(), 19000);
        assert_eq!(AvroValue::create_time_millis(1).get_time_millis().unwrap(), 1);
        assert_eq!(AvroValue::create_time_micros(2).get_time_micros().unwrap(), 2);
        assert_eq!(AvroValue::create_timestamp_millis(3).get_timestamp_millis().unwrap(), 3);
        assert_eq!(AvroValue::create_timestamp_micros(4).get_timestamp_micros().unwrap(), 4);
        assert_eq!(AvroValue::create_timestamp_nanos(5).get_timestamp_nanos().unwrap(), 5);
        assert_eq!(
            AvroValue::create_local_timestamp_millis(6).get_local_timestamp_millis().unwrap(),
            6
        );
        assert_eq!(
            AvroValue::create_local_timestamp_micros(7).get_local_timestamp_micros().unwrap(),
            7
        );
        assert_eq!(
            AvroValue::create_local_timestamp_nanos(8).get_local_timestamp_nanos().unwrap(),
            8
        );

        let duration = AvroValue::create_duration(1, 2, 3);
        assert!(duration.is_duration());
        assert_eq!(duration.get_duration_months().unwrap(), 1);
        assert_eq!(duration.get_duration_days().unwrap(), 2);
        assert_eq!(duration.get_duration_millis().unwrap(), 3);
    }

    #[test]
    fn validate_against_schema() {
        let schema = AvroSchema::parse(b"\"long\"").unwrap();
        assert!(AvroValue::create_long(1).validate(&schema));
        assert!(!AvroValue::create_string(b"no").unwrap().validate(&schema));
    }

    #[test]
    fn resolve_promotes_int_to_long() {
        let long_schema = AvroSchema::parse(b"\"long\"").unwrap();
        let resolved = AvroValue::create_int(41).resolve(&long_schema).unwrap();
        assert_eq!(resolved.get_long().unwrap(), 41);

        let string_schema = AvroSchema::parse(b"\"string\"").unwrap();
        assert!(AvroValue::create_boolean(true).resolve(&string_schema).is_err());
    }

    #[test]
    fn to_json_string_renders_records() {
        let mut record = AvroValue::create_record();
        record.record_put(b"id", &AvroValue::create_long(7)).unwrap();
        record.record_put(b"name", &AvroValue::create_string(b"x").unwrap()).unwrap();
        let json = String::from_utf8(record.to_json_string().unwrap().into_vec()).unwrap();
        let parsed: serde_json::Value = serde_json::from_str(&json).unwrap();
        assert_eq!(parsed["id"], 7);
        assert_eq!(parsed["name"], "x");
    }

    #[test]
    fn type_names() {
        assert_eq!(AvroValue::create_null().type_name().as_slice(), b"null");
        assert_eq!(AvroValue::create_record().type_name().as_slice(), b"record");
        assert_eq!(
            AvroValue::create_timestamp_micros(1).type_name().as_slice(),
            b"timestamp-micros"
        );
    }
}
