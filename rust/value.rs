//! Crubit-friendly wrapper around `apache_avro::types::Value`.
//! Replaces avrocpp's `GenericDatum`.

use crate::make_vec_type;
use crate::schema::{AvroSchema, SchemaType};
use crate::vec_u8::{Status, VecU8, catch_panic, utf8};
use apache_avro::types::Value;
use apache_avro::{Days, Decimal, Duration, Millis, Months};

make_vec_type!(VecU8, VecVecU8);

/// A generic Avro value tree. Replaces avrocpp's `GenericDatum`.
///
/// Values are built with the `create_*` constructors and `record_put` /
/// `array_push` / `map_put` mutators, inspected with `schema_type`, and read
/// with the `get_*` accessors. Accessors return clones; the boundary cannot
/// return references.
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
    // ------------------------------------------------------------------
    // Constructors: primitive types.
    // ------------------------------------------------------------------

    pub fn create_null() -> AvroValue {
        Self { value: Value::Null }
    }

    pub fn create_boolean(v: bool) -> AvroValue {
        Self {
            value: Value::Boolean(v),
        }
    }

    pub fn create_int(v: i32) -> AvroValue {
        Self {
            value: Value::Int(v),
        }
    }

    pub fn create_long(v: i64) -> AvroValue {
        Self {
            value: Value::Long(v),
        }
    }

    pub fn create_float(v: f32) -> AvroValue {
        Self {
            value: Value::Float(v),
        }
    }

    pub fn create_double(v: f64) -> AvroValue {
        Self {
            value: Value::Double(v),
        }
    }

    pub fn create_bytes(v: &[u8]) -> AvroValue {
        Self {
            value: Value::Bytes(v.to_vec()),
        }
    }

    /// Creates a string value. Returns an error if `v` is not valid UTF-8.
    pub fn create_string(v: &[u8]) -> Result<AvroValue, VecU8> {
        Ok(Self {
            value: Value::String(utf8(v)?.to_string()),
        })
    }

    // ------------------------------------------------------------------
    // Constructors: complex types.
    // ------------------------------------------------------------------

    /// Creates an empty record. Add fields with `record_put`.
    pub fn create_record() -> AvroValue {
        Self {
            value: Value::Record(Vec::new()),
        }
    }

    /// Creates an empty array. Add items with `array_push`.
    pub fn create_array() -> AvroValue {
        Self {
            value: Value::Array(Vec::new()),
        }
    }

    /// Creates an empty map. Add entries with `map_put`.
    pub fn create_map() -> AvroValue {
        Self {
            value: Value::Map(Default::default()),
        }
    }

    /// Creates an enum value from the symbol's position in the schema and
    /// the symbol name. Returns an error if `symbol` is not valid UTF-8.
    pub fn create_enum(position: u32, symbol: &[u8]) -> Result<AvroValue, VecU8> {
        Ok(Self {
            value: Value::Enum(position, utf8(symbol)?.to_string()),
        })
    }

    pub fn create_fixed(v: &[u8]) -> AvroValue {
        Self {
            value: Value::Fixed(v.len(), v.to_vec()),
        }
    }

    /// Creates a union value holding `v` at the given zero-based branch
    /// index of the union schema.
    pub fn create_union(branch_index: u32, v: &AvroValue) -> AvroValue {
        Self {
            value: Value::Union(branch_index, Box::new(v.value.clone())),
        }
    }

    // ------------------------------------------------------------------
    // Constructors: logical types.
    // ------------------------------------------------------------------

    /// Creates a decimal value from its two's-complement big-endian
    /// representation (as serialized by Avro).
    pub fn create_decimal(v: &[u8]) -> AvroValue {
        Self {
            value: Value::Decimal(Decimal::from(v)),
        }
    }

    /// Creates a UUID value from its string representation.
    pub fn create_uuid(v: &[u8]) -> Result<AvroValue, VecU8> {
        let uuid = uuid::Uuid::parse_str(utf8(v)?).map_err(|err| VecU8::from(err.to_string()))?;
        Ok(Self {
            value: Value::Uuid(uuid),
        })
    }

    /// Creates a date value (days since the Unix epoch).
    pub fn create_date(days: i32) -> AvroValue {
        Self {
            value: Value::Date(days),
        }
    }

    pub fn create_time_millis(v: i32) -> AvroValue {
        Self {
            value: Value::TimeMillis(v),
        }
    }

    pub fn create_time_micros(v: i64) -> AvroValue {
        Self {
            value: Value::TimeMicros(v),
        }
    }

    pub fn create_timestamp_millis(v: i64) -> AvroValue {
        Self {
            value: Value::TimestampMillis(v),
        }
    }

    pub fn create_timestamp_micros(v: i64) -> AvroValue {
        Self {
            value: Value::TimestampMicros(v),
        }
    }

    pub fn create_timestamp_nanos(v: i64) -> AvroValue {
        Self {
            value: Value::TimestampNanos(v),
        }
    }

    pub fn create_local_timestamp_millis(v: i64) -> AvroValue {
        Self {
            value: Value::LocalTimestampMillis(v),
        }
    }

    pub fn create_local_timestamp_micros(v: i64) -> AvroValue {
        Self {
            value: Value::LocalTimestampMicros(v),
        }
    }

    pub fn create_local_timestamp_nanos(v: i64) -> AvroValue {
        Self {
            value: Value::LocalTimestampNanos(v),
        }
    }

    pub fn create_duration(months: u32, days: u32, millis: u32) -> AvroValue {
        Self {
            value: Value::Duration(Duration::new(
                Months::new(months),
                Days::new(days),
                Millis::new(millis),
            )),
        }
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

    /// Returns the string's raw bytes. Also accepts `Value::Bytes`, which is
    /// what D1 decodes an invalid-UTF-8 `string` to; `get_bytes` deliberately
    /// does not gain the reverse. See doc/specs/AvroStringPolicy.md.
    pub fn get_string(&self) -> Result<VecU8, VecU8> {
        match &self.value {
            Value::String(v) => Ok(v.as_str().into()),
            Value::Bytes(v) => Ok(v.as_slice().into()),
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
            Value::Union(_, v) => Ok(Self {
                value: v.as_ref().clone(),
            }),
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
                Some(item) => Ok(Self {
                    value: item.clone(),
                }),
                None => {
                    Err(format!("Array index {} out of bounds (len {})", index, items.len()).into())
                }
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
                Ok(keys
                    .into_iter()
                    .map(|k| k.as_str().into())
                    .collect::<Vec<VecU8>>()
                    .into())
            }
            _ => Err(self.type_error("map")),
        }
    }

    pub fn get_map_value(&self, raw_key: &[u8]) -> Result<AvroValue, VecU8> {
        let key = utf8(raw_key)?;
        match &self.value {
            Value::Map(entries) => match entries.get(key) {
                Some(v) => Ok(Self { value: v.clone() }),
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
                    Some((_, v)) => Ok(Self { value: v.clone() }),
                    None => Err(format!("Field '{}' not found in record", name).into()),
                }
            }
            _ => Err(self.type_error("record")),
        }
    }

    pub fn has_record_field(&self, raw_name: &[u8]) -> Result<bool, VecU8> {
        let name = utf8(raw_name)?;
        match &self.value {
            Value::Record(fields) => Ok(fields.iter().any(|(field_name, _)| field_name == name)),
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

    /// Returns the value variant without allocating a string.
    pub fn schema_type(&self) -> SchemaType {
        Self::schema_type_of(&self.value)
    }

    fn schema_type_of(value: &Value) -> SchemaType {
        match value {
            Value::Null => SchemaType::Null,
            Value::Boolean(_) => SchemaType::Boolean,
            Value::Int(_) => SchemaType::Int,
            Value::Long(_) => SchemaType::Long,
            Value::Float(_) => SchemaType::Float,
            Value::Double(_) => SchemaType::Double,
            Value::Bytes(_) => SchemaType::Bytes,
            Value::String(_) => SchemaType::String,
            Value::Fixed(_, _) => SchemaType::Fixed,
            Value::Enum(_, _) => SchemaType::Enum,
            Value::Union(_, _) => SchemaType::Union,
            Value::Array(_) => SchemaType::Array,
            Value::Map(_) => SchemaType::Map,
            Value::Record(_) => SchemaType::Record,
            Value::Date(_) => SchemaType::Date,
            Value::Decimal(_) => SchemaType::Decimal,
            Value::BigDecimal(_) => SchemaType::BigDecimal,
            Value::TimeMillis(_) => SchemaType::TimeMillis,
            Value::TimeMicros(_) => SchemaType::TimeMicros,
            Value::TimestampMillis(_) => SchemaType::TimestampMillis,
            Value::TimestampMicros(_) => SchemaType::TimestampMicros,
            Value::TimestampNanos(_) => SchemaType::TimestampNanos,
            Value::LocalTimestampMillis(_) => SchemaType::LocalTimestampMillis,
            Value::LocalTimestampMicros(_) => SchemaType::LocalTimestampMicros,
            Value::LocalTimestampNanos(_) => SchemaType::LocalTimestampNanos,
            Value::Duration(_) => SchemaType::Duration,
            Value::Uuid(_) => SchemaType::Uuid,
        }
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
            Ok(value) => Ok(Self { value }),
            Err(err) => Err(err.to_string().into()),
        })
    }

    /// Converts this value to a JSON string. Bytes and fixed values are
    /// rendered as arrays of numbers, following apache-avro's conversion.
    pub fn to_json_string(&self) -> Result<VecU8, VecU8> {
        catch_panic(|| {
            let json = serde_json::Value::try_from(self.value.clone())
                .map_err(|err| VecU8::from(err.to_string()))?;
            serde_json::to_string(&json)
                .map(VecU8::from)
                .map_err(|err| err.to_string().into())
        })
    }

    /// Compares two values for equality.
    pub fn equals(&self, other: &AvroValue) -> bool {
        self.value == other.value
    }

    fn schema_type_name(&self) -> &'static str {
        self.schema_type().name()
    }

    fn type_error(&self, expected: &str) -> VecU8 {
        format!(
            "Value is not {}; actual type is {}",
            expected,
            self.schema_type_name()
        )
        .into()
    }
}

make_vec_type!(AvroValue, VecAvroValue);

// ---------------------------------------------------------------------------
// Path navigation
// ---------------------------------------------------------------------------

/// One navigation step: a record field, an array index, or a map key.
#[derive(Debug, Clone, PartialEq)]
enum Step {
    Field(String),
    Index(usize),
    Key(String),
}

/// A path from the root of an `AvroValue` to somewhere inside it, used by the
/// `*_at` accessors to read a leaf without copying anything on the way.
///
/// The plain accessors (`get_record_field`, `get_array_item`, ...) must return
/// an owned `AvroValue`, because the FFI boundary cannot hand back a
/// reference; each one therefore clones the subtree it returns. Reaching
/// `items[3].count` that way copies the whole `items` array, then copies
/// `items[3]` again. Reading every leaf of a record holding an eight-item
/// array of sub-records and a four-key map costs more than decoding that
/// record did.
///
/// A path avoids all of it: build the path once, and each read walks it
/// against the borrowed tree and copies only the leaf. Reuse one path across
/// a loop with `set_last_index` rather than rebuilding it per iteration.
///
/// Use a path when reaching a leaf would otherwise copy a composite subtree.
/// Do *not* reach for one to read a top-level scalar: a path read costs an
/// extra push and pop, and for a two-field flat record reading both fields by
/// path measures around 45% slower than `get_record_field` plus `get_long`,
/// because there is no subtree worth avoiding. Measured on the same 10000
/// records (benchmarks/access_probe.cc), the cost of reading every leaf:
///
/// | record                        | cloning accessors | by path |
/// |-------------------------------|-------------------|---------|
/// | flat, two scalar fields       |             78 ns |  112 ns |
/// | 8-item array + 4-key map      |           1975 ns | 1085 ns |
#[derive(Debug, Clone, Default)]
pub struct AvroPath {
    /// `steps[..len]` is the path. Steps past `len` have been popped but are
    /// kept so a later push can reuse their string allocation: the loop that
    /// walks an array pushes and pops a field name per item, and allocating
    /// one each time costs more than the copy the path exists to avoid.
    steps: Vec<Step>,
    len: usize,
}

/// Two paths are equal when they name the same location; the popped steps
/// kept behind for reuse are not part of that.
impl PartialEq for AvroPath {
    fn eq(&self, other: &Self) -> bool {
        self.steps[..self.len] == other.steps[..other.len]
    }
}

impl AvroPath {
    /// Creates an empty path, which refers to the value's root. (Named
    /// `create` rather than `new` because `new` is reserved in the generated
    /// C++.)
    pub fn create() -> AvroPath {
        AvroPath {
            steps: Vec::new(),
            len: 0,
        }
    }

    /// Appends a record field name. Errors if `name` is not valid UTF-8.
    pub fn push_field(&mut self, name: &[u8]) -> Status {
        let name = utf8(name)?;
        self.push(|text| Step::Field(text), name);
        Ok(0)
    }

    /// Appends an array index.
    pub fn push_index(&mut self, index: usize) {
        if self.len < self.steps.len() {
            self.steps[self.len] = Step::Index(index);
        } else {
            self.steps.push(Step::Index(index));
        }
        self.len += 1;
    }

    /// Appends a map key. Errors if `key` is not valid UTF-8.
    pub fn push_key(&mut self, key: &[u8]) -> Status {
        let key = utf8(key)?;
        self.push(|text| Step::Key(text), key);
        Ok(0)
    }

    /// Removes the last step. Errors if the path is already empty. The step
    /// stays in `steps` so the next push can reuse its allocation.
    pub fn pop(&mut self) -> Status {
        if self.len == 0 {
            return Err("Path is already empty".into());
        }
        self.len -= 1;
        Ok(0)
    }

    /// Appends a text step, reusing the popped slot's string if there is one.
    fn push(&mut self, make: fn(String) -> Step, text: &str) {
        if self.len < self.steps.len() {
            // Reuse the popped step's buffer rather than allocating.
            let mut recycled = match std::mem::replace(&mut self.steps[self.len], Step::Index(0)) {
                Step::Field(buffer) | Step::Key(buffer) => buffer,
                Step::Index(_) => String::new(),
            };
            recycled.clear();
            recycled.push_str(text);
            self.steps[self.len] = make(recycled);
        } else {
            self.steps.push(make(text.to_string()));
        }
        self.len += 1;
    }

    /// Replaces the last step with an array index, so a loop can walk an
    /// array without rebuilding the path (or reallocating) each iteration.
    /// Errors if the path is empty or does not end in an index.
    pub fn set_last_index(&mut self, index: usize) -> Status {
        match self.steps[..self.len].last_mut() {
            Some(Step::Index(last)) => {
                *last = index;
                Ok(0)
            }
            Some(_) => Err("Last path step is not an array index".into()),
            None => Err("Path is empty".into()),
        }
    }

    /// Empties the path, keeping the step buffers for reuse.
    pub fn clear(&mut self) {
        self.len = 0;
    }

    /// (Returns u64 because Crubit cannot bridge `usize` returns uniformly
    /// with the rest of this API.)
    pub fn len(&self) -> u64 {
        self.len as u64
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Renders the path for error messages, e.g. `items[3].count`.
    fn describe(&self) -> String {
        let mut out = String::new();
        for step in &self.steps[..self.len] {
            match step {
                Step::Field(name) => {
                    if !out.is_empty() {
                        out.push('.');
                    }
                    out.push_str(name);
                }
                Step::Index(index) => out.push_str(&format!("[{}]", index)),
                Step::Key(key) => out.push_str(&format!("[\"{}\"]", key)),
            }
        }
        if out.is_empty() {
            "<root>".to_string()
        } else {
            out
        }
    }
}

impl AvroValue {
    /// Borrows the value at `path`, walking the tree without copying.
    /// Unions are transparent along the way: a caller reading
    /// `contact.email` through a nullable `contact` means the branch the
    /// union holds. The value the path lands on is returned as-is, union
    /// included; `at_leaf` is the variant that unwraps it.
    fn at<'a>(&'a self, path: &AvroPath) -> Result<&'a Value, VecU8> {
        let mut current = &self.value;
        for (depth, step) in path.steps[..path.len].iter().enumerate() {
            current = Self::unwrap_unions(current);
            let next = match (step, current) {
                (Step::Field(name), Value::Record(fields)) => fields
                    .iter()
                    .find(|(field, _)| field == name)
                    .map(|(_, v)| v),
                (Step::Index(index), Value::Array(items)) => items.get(*index),
                (Step::Key(key), Value::Map(entries)) => entries.get(key),
                _ => None,
            };
            match next {
                Some(value) => current = value,
                None => return Err(Self::path_error(path, depth)),
            }
        }
        Ok(current)
    }

    /// `at`, then unwraps a union the path landed directly on, so reading a
    /// nullable long yields the long (or null) rather than the wrapper.
    fn at_leaf<'a>(&'a self, path: &AvroPath) -> Result<&'a Value, VecU8> {
        Ok(Self::unwrap_unions(self.at(path)?))
    }

    fn unwrap_unions(mut value: &Value) -> &Value {
        while let Value::Union(_, inner) = value {
            value = inner.as_ref();
        }
        value
    }

    fn path_error(path: &AvroPath, depth: usize) -> VecU8 {
        format!(
            "No value at path {} (failed at step {} of {})",
            path.describe(),
            depth + 1,
            path.len
        )
        .into()
    }

    fn at_type_error(path: &AvroPath, actual: &Value, expected: &str) -> VecU8 {
        format!(
            "Value at path {} is not {}; actual type is {}",
            path.describe(),
            expected,
            AvroValue {
                value: actual.clone()
            }
            .schema_type_name()
        )
        .into()
    }

    // ------------------------------------------------------------------
    // Leaf reads at a path. Each copies the leaf and nothing else.
    // ------------------------------------------------------------------

    pub fn get_boolean_at(&self, path: &AvroPath) -> Result<bool, VecU8> {
        match self.at_leaf(path)? {
            Value::Boolean(v) => Ok(*v),
            other => Err(Self::at_type_error(path, other, "boolean")),
        }
    }

    pub fn get_int_at(&self, path: &AvroPath) -> Result<i32, VecU8> {
        match self.at_leaf(path)? {
            Value::Int(v) => Ok(*v),
            other => Err(Self::at_type_error(path, other, "int")),
        }
    }

    pub fn get_long_at(&self, path: &AvroPath) -> Result<i64, VecU8> {
        match self.at_leaf(path)? {
            Value::Long(v) => Ok(*v),
            other => Err(Self::at_type_error(path, other, "long")),
        }
    }

    pub fn get_float_at(&self, path: &AvroPath) -> Result<f32, VecU8> {
        match self.at_leaf(path)? {
            Value::Float(v) => Ok(*v),
            other => Err(Self::at_type_error(path, other, "float")),
        }
    }

    pub fn get_double_at(&self, path: &AvroPath) -> Result<f64, VecU8> {
        match self.at_leaf(path)? {
            Value::Double(v) => Ok(*v),
            other => Err(Self::at_type_error(path, other, "double")),
        }
    }

    /// Also accepts `Value::Bytes`; see `get_string`.
    pub fn get_string_at(&self, path: &AvroPath) -> Result<VecU8, VecU8> {
        match self.at_leaf(path)? {
            Value::String(v) => Ok(v.as_str().into()),
            Value::Bytes(v) => Ok(v.as_slice().into()),
            other => Err(Self::at_type_error(path, other, "string")),
        }
    }

    pub fn get_bytes_at(&self, path: &AvroPath) -> Result<VecU8, VecU8> {
        match self.at_leaf(path)? {
            Value::Bytes(v) => Ok(v.as_slice().into()),
            other => Err(Self::at_type_error(path, other, "bytes")),
        }
    }

    pub fn is_null_at(&self, path: &AvroPath) -> Result<bool, VecU8> {
        Ok(matches!(self.at_leaf(path)?, Value::Null))
    }

    /// Returns the schema variant at `path`.
    pub fn schema_type_at(&self, path: &AvroPath) -> Result<SchemaType, VecU8> {
        Ok(Self::schema_type_of(self.at_leaf(path)?))
    }

    // ------------------------------------------------------------------
    // Container sizes at a path, for driving loops without copying the
    // container. (u64 for the same reason as get_array_len.)
    // ------------------------------------------------------------------

    pub fn get_array_len_at(&self, path: &AvroPath) -> Result<u64, VecU8> {
        match self.at_leaf(path)? {
            Value::Array(items) => Ok(items.len() as u64),
            other => Err(Self::at_type_error(path, other, "array")),
        }
    }

    pub fn get_map_len_at(&self, path: &AvroPath) -> Result<u64, VecU8> {
        match self.at_leaf(path)? {
            Value::Map(entries) => Ok(entries.len() as u64),
            other => Err(Self::at_type_error(path, other, "map")),
        }
    }

    pub fn get_record_len_at(&self, path: &AvroPath) -> Result<u64, VecU8> {
        match self.at_leaf(path)? {
            Value::Record(fields) => Ok(fields.len() as u64),
            other => Err(Self::at_type_error(path, other, "record")),
        }
    }

    /// Returns the keys of the map at `path`, sorted lexicographically.
    pub fn get_map_keys_at(&self, path: &AvroPath) -> Result<VecVecU8, VecU8> {
        match self.at_leaf(path)? {
            Value::Map(entries) => {
                let mut keys: Vec<&String> = entries.keys().collect();
                keys.sort();
                Ok(keys
                    .into_iter()
                    .map(|k| k.as_str().into())
                    .collect::<Vec<VecU8>>()
                    .into())
            }
            other => Err(Self::at_type_error(path, other, "map")),
        }
    }

    /// Returns the field names of the record at `path`, in field order.
    pub fn get_record_field_names_at(&self, path: &AvroPath) -> Result<VecVecU8, VecU8> {
        match self.at_leaf(path)? {
            Value::Record(fields) => Ok(fields
                .iter()
                .map(|(name, _)| name.as_str().into())
                .collect::<Vec<VecU8>>()
                .into()),
            other => Err(Self::at_type_error(path, other, "record")),
        }
    }

    /// Copies out the subtree at `path`. This is the escape hatch for
    /// callers that really do want an owned value; the `*_at` leaf readers
    /// above exist so that reaching a leaf does not have to.
    pub fn get_value_at(&self, path: &AvroPath) -> Result<AvroValue, VecU8> {
        Ok(AvroValue {
            value: self.at(path)?.clone(),
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn primitive_roundtrips() {
        assert_eq!(AvroValue::create_null().schema_type(), SchemaType::Null);
        assert!(AvroValue::create_boolean(true).get_boolean().unwrap());
        assert_eq!(AvroValue::create_int(-42).get_int().unwrap(), -42);
        assert_eq!(AvroValue::create_long(1 << 40).get_long().unwrap(), 1 << 40);
        assert_eq!(AvroValue::create_float(1.5).get_float().unwrap(), 1.5);
        assert_eq!(AvroValue::create_double(2.5).get_double().unwrap(), 2.5);
        assert_eq!(
            AvroValue::create_bytes(&[1, 2, 3])
                .get_bytes()
                .unwrap()
                .as_slice(),
            &[1, 2, 3]
        );
        assert_eq!(
            AvroValue::create_string(b"hello")
                .unwrap()
                .get_string()
                .unwrap()
                .as_slice(),
            b"hello"
        );
    }

    #[test]
    fn default_is_null() {
        assert_eq!(AvroValue::default().schema_type(), SchemaType::Null);
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

    /// D1 is a read-path decision: `create_string` above still rejects, so the
    /// `Value::Bytes` here is hand-built.
    #[test]
    fn get_string_accepts_bytes_but_get_bytes_does_not_accept_string() {
        let invalid_string_field = AvroValue {
            value: Value::Bytes(vec![0xff, 0xfe]),
        };
        assert_eq!(
            invalid_string_field.get_string().unwrap().as_slice(),
            &[0xff, 0xfe]
        );
        assert_eq!(
            invalid_string_field.get_bytes().unwrap().as_slice(),
            &[0xff, 0xfe]
        );

        let root = AvroPath::create();
        assert_eq!(
            invalid_string_field
                .get_string_at(&root)
                .unwrap()
                .as_slice(),
            &[0xff, 0xfe]
        );
        assert_eq!(
            invalid_string_field.get_bytes_at(&root).unwrap().as_slice(),
            &[0xff, 0xfe]
        );

        // One-directional: a real String is never handed back by get_bytes.
        let actual_string = AvroValue::create_string(b"hi").unwrap();
        assert!(actual_string.get_bytes().is_err());
        assert!(actual_string.get_bytes_at(&root).is_err());
    }

    #[test]
    fn enum_roundtrip() {
        let v = AvroValue::create_enum(2, b"GREEN").unwrap();
        assert_eq!(v.schema_type(), SchemaType::Enum);
        assert_eq!(v.get_enum_position().unwrap(), 2);
        assert_eq!(v.get_enum_symbol().unwrap().as_slice(), b"GREEN");
    }

    #[test]
    fn fixed_roundtrip() {
        let v = AvroValue::create_fixed(&[9, 8, 7, 6]);
        assert_eq!(v.schema_type(), SchemaType::Fixed);
        assert_eq!(v.get_fixed_bytes().unwrap().as_slice(), &[9, 8, 7, 6]);
    }

    #[test]
    fn union_roundtrip() {
        let inner = AvroValue::create_long(7);
        let v = AvroValue::create_union(1, &inner);
        assert_eq!(v.schema_type(), SchemaType::Union);
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
        assert_eq!(
            record.get_record_field(b"a").unwrap().get_int().unwrap(),
            10
        );

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
        assert!(
            AvroValue::create_int(0)
                .array_push(&AvroValue::create_null())
                .is_err()
        );
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
        assert_eq!(decimal.schema_type(), SchemaType::Decimal);
        assert_eq!(decimal.get_decimal_bytes().unwrap().as_slice(), &[1, 24]);

        let uuid = AvroValue::create_uuid(b"6f2b0e76-4d3d-4f8e-9d3a-2e1b8a7c6d5e").unwrap();
        assert_eq!(uuid.schema_type(), SchemaType::Uuid);
        assert_eq!(
            uuid.get_uuid().unwrap().as_slice(),
            b"6f2b0e76-4d3d-4f8e-9d3a-2e1b8a7c6d5e"
        );
        assert!(AvroValue::create_uuid(b"not-a-uuid").is_err());

        assert_eq!(AvroValue::create_date(19000).get_date().unwrap(), 19000);
        assert_eq!(
            AvroValue::create_time_millis(1).get_time_millis().unwrap(),
            1
        );
        assert_eq!(
            AvroValue::create_time_micros(2).get_time_micros().unwrap(),
            2
        );
        assert_eq!(
            AvroValue::create_timestamp_millis(3)
                .get_timestamp_millis()
                .unwrap(),
            3
        );
        assert_eq!(
            AvroValue::create_timestamp_micros(4)
                .get_timestamp_micros()
                .unwrap(),
            4
        );
        assert_eq!(
            AvroValue::create_timestamp_nanos(5)
                .get_timestamp_nanos()
                .unwrap(),
            5
        );
        assert_eq!(
            AvroValue::create_local_timestamp_millis(6)
                .get_local_timestamp_millis()
                .unwrap(),
            6
        );
        assert_eq!(
            AvroValue::create_local_timestamp_micros(7)
                .get_local_timestamp_micros()
                .unwrap(),
            7
        );
        assert_eq!(
            AvroValue::create_local_timestamp_nanos(8)
                .get_local_timestamp_nanos()
                .unwrap(),
            8
        );

        let duration = AvroValue::create_duration(1, 2, 3);
        assert_eq!(duration.schema_type(), SchemaType::Duration);
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
        assert!(
            AvroValue::create_boolean(true)
                .resolve(&string_schema)
                .is_err()
        );
    }

    #[test]
    fn to_json_string_renders_records() {
        let mut record = AvroValue::create_record();
        record
            .record_put(b"id", &AvroValue::create_long(7))
            .unwrap();
        record
            .record_put(b"name", &AvroValue::create_string(b"x").unwrap())
            .unwrap();
        let json = String::from_utf8(record.to_json_string().unwrap().into_vec()).unwrap();
        let parsed: serde_json::Value = serde_json::from_str(&json).unwrap();
        assert_eq!(parsed["id"], 7);
        assert_eq!(parsed["name"], "x");
    }

    /// {items: [{key, count} x2], tags: {a: 1}, maybe: union(null|long)}
    fn nested_fixture(maybe: Value) -> AvroValue {
        let item = |key: &str, count: i64| {
            Value::Record(vec![
                ("key".to_string(), Value::String(key.to_string())),
                ("count".to_string(), Value::Long(count)),
            ])
        };
        AvroValue {
            value: Value::Record(vec![
                (
                    "items".to_string(),
                    Value::Array(vec![item("first", 10), item("second", 20)]),
                ),
                (
                    "tags".to_string(),
                    Value::Map([("a".to_string(), Value::Long(7))].into_iter().collect()),
                ),
                ("maybe".to_string(), maybe),
            ]),
        }
    }

    #[test]
    fn path_reads_leaves_without_copying_the_way_there() {
        let record = nested_fixture(Value::Null);
        let mut path = AvroPath::create();
        path.push_field(b"items").unwrap();
        assert_eq!(record.get_array_len_at(&path).unwrap(), 2);

        path.push_index(1);
        path.push_field(b"key").unwrap();
        assert_eq!(record.get_string_at(&path).unwrap().as_slice(), b"second");

        // Reuse the path across the array rather than rebuilding it.
        path.pop().unwrap();
        path.push_field(b"count").unwrap();
        assert_eq!(record.get_long_at(&path).unwrap(), 20);
        path.pop().unwrap();
        path.set_last_index(0).unwrap();
        path.push_field(b"count").unwrap();
        assert_eq!(record.get_long_at(&path).unwrap(), 10);

        // Map access by key.
        let mut tags = AvroPath::create();
        tags.push_field(b"tags").unwrap();
        assert_eq!(record.get_map_len_at(&tags).unwrap(), 1);
        assert_eq!(tags.len(), 1);
        tags.push_key(b"a").unwrap();
        assert_eq!(record.get_long_at(&tags).unwrap(), 7);

        // An empty path is the root.
        let root = AvroPath::create();
        assert!(root.is_empty());
        assert_eq!(record.get_record_len_at(&root).unwrap(), 3);
        assert_eq!(record.schema_type_at(&root).unwrap(), SchemaType::Record);
    }

    #[test]
    fn path_sees_through_unions() {
        // Reading a nullable field yields the branch, not the wrapper.
        let present = nested_fixture(Value::Union(1, Box::new(Value::Long(99))));
        let mut path = AvroPath::create();
        path.push_field(b"maybe").unwrap();
        assert_eq!(present.get_long_at(&path).unwrap(), 99);
        assert!(!present.is_null_at(&path).unwrap());

        let absent = nested_fixture(Value::Union(0, Box::new(Value::Null)));
        assert!(absent.is_null_at(&path).unwrap());
        assert!(absent.get_long_at(&path).is_err());

        // Navigating *through* a union reaches into the branch, so a step
        // after the union is applied to what it holds and not skipped.
        let wrapped = AvroValue {
            value: Value::Record(vec![(
                "contact".to_string(),
                Value::Union(
                    1,
                    Box::new(Value::Record(vec![(
                        "email".to_string(),
                        Value::String("x@y".to_string()),
                    )])),
                ),
            )]),
        };
        let mut through = AvroPath::create();
        through.push_field(b"contact").unwrap();
        through.push_field(b"email").unwrap();
        assert_eq!(wrapped.get_string_at(&through).unwrap().as_slice(), b"x@y");

        // get_value_at keeps the union itself, so it stays inspectable.
        let mut contact = AvroPath::create();
        contact.push_field(b"contact").unwrap();
        assert_eq!(
            wrapped.get_value_at(&contact).unwrap().schema_type(),
            SchemaType::Union
        );
    }

    #[test]
    fn path_errors_name_the_path_and_the_failing_step() {
        let record = nested_fixture(Value::Null);
        let mut missing = AvroPath::create();
        missing.push_field(b"items").unwrap();
        missing.push_index(9);
        let err = String::from_utf8(record.get_long_at(&missing).unwrap_err().into_vec()).unwrap();
        assert!(err.contains("items[9]"), "{err}");
        assert!(err.contains("step 2 of 2"), "{err}");

        // Right path, wrong leaf type.
        let mut wrong_type = AvroPath::create();
        wrong_type.push_field(b"items").unwrap();
        let err =
            String::from_utf8(record.get_long_at(&wrong_type).unwrap_err().into_vec()).unwrap();
        assert!(err.contains("not long"), "{err}");
        assert!(err.contains("array"), "{err}");

        // Stepping into a scalar fails rather than silently returning it.
        let mut into_scalar = AvroPath::create();
        into_scalar.push_field(b"tags").unwrap();
        into_scalar.push_key(b"a").unwrap();
        into_scalar.push_field(b"nope").unwrap();
        assert!(record.get_long_at(&into_scalar).is_err());
    }

    #[test]
    fn path_editing_is_checked() {
        let mut path = AvroPath::create();
        assert!(path.pop().is_err());
        assert!(path.set_last_index(0).is_err());
        path.push_field(b"items").unwrap();
        // The last step is a field, not an index.
        assert!(path.set_last_index(0).is_err());
        path.push_index(3);
        assert!(path.set_last_index(4).is_ok());
        assert_eq!(path.len(), 2);
        path.clear();
        assert!(path.is_empty());
        assert!(path.push_field(&[0xff]).is_err());
        assert!(path.push_key(&[0xff]).is_err());
    }

    #[test]
    fn path_and_plain_accessors_agree() {
        let record = nested_fixture(Value::Null);
        let mut path = AvroPath::create();
        path.push_field(b"items").unwrap();
        path.push_index(0);
        path.push_field(b"key").unwrap();

        let by_clone = record
            .get_record_field(b"items")
            .unwrap()
            .get_array_item(0)
            .unwrap()
            .get_record_field(b"key")
            .unwrap()
            .get_string()
            .unwrap();
        assert_eq!(
            record.get_string_at(&path).unwrap().as_slice(),
            by_clone.as_slice()
        );
    }

    #[test]
    fn schema_types() {
        assert_eq!(AvroValue::create_null().schema_type(), SchemaType::Null);
        assert_eq!(AvroValue::create_record().schema_type(), SchemaType::Record);
        assert_eq!(
            AvroValue::create_timestamp_micros(1).schema_type(),
            SchemaType::TimestampMicros
        );
    }
}
