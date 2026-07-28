//! Projected decode: materialize only the fields a caller asked for, and
//! advance past the rest without allocating.
//!
//! Why this exists, and why it is not `DataFileReader::with_reader_schema`:
//! apache-avro's schema resolution decodes the *whole* writer-schema value
//! and then resolves it, so asking it for three fields out of twenty costs
//! more than asking for all twenty (measured in rust/tests/manifest_alloc.rs:
//! 691 allocator operations projected against 670 unprojected). avro-cpp's
//! ResolvingDecoder skips unread fields at the byte level and is 2.4x-3.6x
//! faster for it. See doc/specs/AvroTokenStream.md for the measurements that
//! motivated this module.
//!
//! What this module owns: walking the Avro binary encoding to know how many
//! bytes a value occupies, so unwanted subtrees can be stepped over. What it
//! does not own: schema parsing, container framing, and the construction of
//! values whose types need apache-avro internals (see `Plan::Delegate`).
//!
//! ## Allocation bounds
//!
//! No new limit is introduced and `max_allocation_bytes` is not
//! reimplemented. Instead this module relies on a structural invariant: a
//! container block holds a whole number of complete datums, so any length
//! prefix larger than the bytes remaining in the containing buffer is
//! malformed by definition. `check_len` enforces exactly that, which means no
//! attacker-controlled length is ever honored. Block size itself is already
//! capped upstream of here by `DataFileReader::set_max_block_size`.

use std::collections::HashMap;

use apache_avro::Duration;
use apache_avro::from_avro_datum_schemata;
use apache_avro::schema::{InnerDecimalSchema, Name, Schema, UuidSchema};
use apache_avro::types::Value;
use uuid::Uuid;

use crate::schema::AvroSchema;
use crate::value::AvroValue;
use crate::vec_u8::{VecU8, catch_panic};

/// Error returned when a projected datum decode leaves bytes unconsumed.
/// Mirrors `crate::datum`: a correctly framed datum buffer is fully consumed.
const TRAILING_BYTES_ERROR: &str = "trailing bytes after single Avro datum";

/// A single array or map block may not claim more items than this. Real Avro
/// writers never approach it (a block is capped at 128 MiB by
/// `set_max_block_size`); it exists so a corrupt count cannot make the
/// decoder allocate without bound when the item type is zero-width.
const MAX_BLOCK_ITEMS: i64 = 1 << 26;

const TRUNCATED: &str = "Truncated Avro datum";

// ---------------------------------------------------------------------------
// Compiled plan
// ---------------------------------------------------------------------------

/// What the projection does with one node of the writer schema.
///
/// Compiled once per (writer schema, projection) pair rather than consulted
/// per value, which is what removes the per-leaf field-name string
/// comparison that makes `AvroValue::at` traversal expensive.
#[derive(Debug, Clone)]
enum Plan {
    /// Materialize the whole subtree here in this module.
    Take,
    /// Step over the subtree, allocating nothing.
    Drop,
    /// Materialize the subtree by handing it to apache-avro. Used for the
    /// types this module deliberately does not construct itself: decimals
    /// (their `Value` construction needs upstream helpers that are
    /// `pub(crate)`) and named references (which need upstream's name
    /// resolution). Chosen at compile time, so a projection that avoids
    /// these types never pays for them.
    Delegate,
    /// A record where only some fields survive. One entry per *writer*
    /// field, in writer order, because the bytes must be walked in that
    /// order whether or not a field is kept.
    Record {
        fields: Vec<FieldPlan>,
        kept: usize,
    },
    Array(Box<Plan>),
    Map(Box<Plan>),
    /// One plan per union variant, in writer order.
    Union(Vec<Plan>),
}

#[derive(Debug, Clone)]
struct FieldPlan {
    /// The name to record in the produced value. `None` means the field is
    /// stepped over and discarded.
    name: Option<String>,
    plan: Plan,
}

/// A projection compiled against a specific writer schema.
///
/// Compiling walks the writer schema and clones it, so it is deliberately a
/// value the caller holds and reuses rather than something rebuilt per datum.
/// `DataFileReader::with_projection` compiles one per file; callers decoding a
/// stream of bare datums should compile once and call `decode_datum` in the
/// loop.
#[derive(Debug, Clone)]
pub struct AvroProjection {
    plan: Plan,
    /// Named types of the writer schema, so `Schema::Ref` can be resolved
    /// while stepping over bytes. Built once at compile time.
    names: HashMap<Name, Schema>,
    /// Kept so delegated subtrees can resolve named references.
    root: Schema,
}

impl Default for AvroProjection {
    fn default() -> Self {
        // Exists for Crubit's move semantics: a C++ move replaces the
        // moved-from object with Default::default(). An identity projection
        // of the null schema decodes nothing, which is the safest inert state.
        AvroProjection {
            plan: Plan::Take,
            names: HashMap::new(),
            root: Schema::Null,
        }
    }
}

impl AvroProjection {
    /// Compiles a projection for use from C++. `projection` must be a subset
    /// of `writer_schema`: the same shape and names, with record fields
    /// optionally omitted.
    ///
    /// (Named `create` rather than `new` because `new` is reserved in the
    /// generated C++.)
    pub fn create(
        writer_schema: &AvroSchema,
        projection: &AvroSchema,
    ) -> Result<AvroProjection, VecU8> {
        catch_panic(|| {
            Self::compile(&writer_schema.schema, &projection.schema).map_err(VecU8::from)
        })
    }

    /// Decodes one datum, materializing only the projected fields and
    /// stepping over the rest. Errors on trailing bytes, as
    /// `crate::datum::decode_datum` does.
    pub fn decode_datum(&self, data: &[u8]) -> Result<AvroValue, VecU8> {
        catch_panic(|| {
            let mut input = data;
            let value = self.decode(&mut input).map_err(VecU8::from)?;
            if !input.is_empty() {
                return Err(TRAILING_BYTES_ERROR.into());
            }
            Ok(AvroValue { value })
        })
    }

    /// Compiles `projection` against `writer`. `projection` must be a subset
    /// of `writer`: same shape, same names, with record fields optionally
    /// omitted. Type promotion and defaults for missing fields are
    /// deliberately not supported -- that is schema resolution, and
    /// conflating "resolve to a different schema" with "decode less of this
    /// schema" would make both harder to reason about.
    pub(crate) fn compile(writer: &Schema, projection: &Schema) -> Result<AvroProjection, String> {
        let plan = compile_node(writer, projection, &mut Vec::new())?;
        let mut names = HashMap::new();
        collect_names(writer, &mut names);
        Ok(AvroProjection {
            plan,
            names,
            root: writer.clone(),
        })
    }

    /// Decodes one datum from `input`, consuming exactly its bytes.
    pub(crate) fn decode(&self, input: &mut &[u8]) -> Result<Value, String> {
        let context = Context {
            names: &self.names,
            root: &self.root,
        };
        context.read(&self.plan, &self.root, input)
    }
}

// ---------------------------------------------------------------------------
// Compilation
// ---------------------------------------------------------------------------

/// Describes where a mismatch was found, so the error names the field rather
/// than just the type.
fn path_of(path: &[String]) -> String {
    if path.is_empty() {
        "<root>".to_string()
    } else {
        path.join(".")
    }
}

fn compile_node(
    writer: &Schema,
    projection: &Schema,
    path: &mut Vec<String>,
) -> Result<Plan, String> {
    // A named reference or a decimal anywhere in a kept subtree means
    // apache-avro builds it. Checked before the structural match so the
    // decision is made once per subtree rather than per value.
    //
    // Delegating hands the whole subtree to apache-avro, which returns every
    // field in it. So this is only sound when the projection asks for the
    // subtree unchanged; narrowing inside it would silently yield more fields
    // than the caller requested. Equality is the check, not `same_kind`,
    // which compares only the outer type and a record's name.
    if !self_contained(writer) {
        if writer == projection {
            return Ok(Plan::Delegate);
        }
        return Err(format!(
            "Projection at {} narrows a subtree containing a decimal or a named \
             type reference, which this decoder hands to apache-avro whole. \
             Request that subtree unchanged, or project around it.",
            path_of(path)
        ));
    }

    match (writer, projection) {
        (Schema::Record(writer_record), Schema::Record(projected_record)) => {
            if writer_record.name != projected_record.name {
                return Err(format!(
                    "Projection at {} names record {} but the writer schema has {}",
                    path_of(path),
                    projected_record.name.fullname(None),
                    writer_record.name.fullname(None)
                ));
            }
            // Every projected field must exist in the writer schema;
            // otherwise the projection is asking for data that is not there,
            // which is a caller error rather than something to fill with a
            // default.
            for projected_field in &projected_record.fields {
                if !writer_record.lookup.contains_key(&projected_field.name) {
                    return Err(format!(
                        "Projection at {} asks for field {}, which the writer schema does not have",
                        path_of(path),
                        projected_field.name
                    ));
                }
            }

            let mut fields = Vec::with_capacity(writer_record.fields.len());
            let mut kept = 0;
            for writer_field in &writer_record.fields {
                let wanted = projected_record
                    .lookup
                    .get(&writer_field.name)
                    .map(|index| &projected_record.fields[*index]);
                match wanted {
                    Some(projected_field) => {
                        path.push(writer_field.name.clone());
                        let plan =
                            compile_node(&writer_field.schema, &projected_field.schema, path)?;
                        path.pop();
                        kept += 1;
                        fields.push(FieldPlan {
                            name: Some(writer_field.name.clone()),
                            plan,
                        });
                    }
                    None => fields.push(FieldPlan {
                        name: None,
                        plan: Plan::Drop,
                    }),
                }
            }
            Ok(Plan::Record { fields, kept })
        }

        (Schema::Array(writer_array), Schema::Array(projected_array)) => {
            path.push("[]".to_string());
            let items = compile_node(&writer_array.items, &projected_array.items, path)?;
            path.pop();
            Ok(Plan::Array(Box::new(items)))
        }

        (Schema::Map(writer_map), Schema::Map(projected_map)) => {
            path.push("{}".to_string());
            let values = compile_node(&writer_map.types, &projected_map.types, path)?;
            path.pop();
            Ok(Plan::Map(Box::new(values)))
        }

        (Schema::Union(writer_union), Schema::Union(projected_union)) => {
            let writer_variants = writer_union.variants();
            let projected_variants = projected_union.variants();
            // Branch indices are written into the data, so a projection
            // cannot drop or reorder branches without changing what the
            // encoded index means.
            if writer_variants.len() != projected_variants.len() {
                return Err(format!(
                    "Projection at {} has {} union branches but the writer schema has {}; \
                     a projection may narrow records, not unions",
                    path_of(path),
                    projected_variants.len(),
                    writer_variants.len()
                ));
            }
            let mut plans = Vec::with_capacity(writer_variants.len());
            for (index, (writer_variant, projected_variant)) in
                writer_variants.iter().zip(projected_variants).enumerate()
            {
                path.push(format!("<branch {index}>"));
                plans.push(compile_node(writer_variant, projected_variant, path)?);
                path.pop();
            }
            Ok(Plan::Union(plans))
        }

        _ if same_kind(writer, projection) => Ok(Plan::Take),
        _ => Err(mismatch(writer, projection, path)),
    }
}

fn mismatch(writer: &Schema, projection: &Schema, path: &[String]) -> String {
    format!(
        "Projection at {} is {:?} but the writer schema is {:?}; a projection must \
         match the writer schema's shape and may only omit record fields",
        path_of(path),
        kind_name(projection),
        kind_name(writer)
    )
}

fn kind_name(schema: &Schema) -> &'static str {
    match schema {
        Schema::Null => "null",
        Schema::Boolean => "boolean",
        Schema::Int => "int",
        Schema::Long => "long",
        Schema::Float => "float",
        Schema::Double => "double",
        Schema::Bytes => "bytes",
        Schema::String => "string",
        Schema::Array(_) => "array",
        Schema::Map(_) => "map",
        Schema::Union(_) => "union",
        Schema::Record(_) => "record",
        Schema::Enum(_) => "enum",
        Schema::Fixed(_) => "fixed",
        Schema::Decimal(_) => "decimal",
        Schema::BigDecimal => "big-decimal",
        Schema::Uuid(_) => "uuid",
        Schema::Date => "date",
        Schema::TimeMillis => "time-millis",
        Schema::TimeMicros => "time-micros",
        Schema::TimestampMillis => "timestamp-millis",
        Schema::TimestampMicros => "timestamp-micros",
        Schema::TimestampNanos => "timestamp-nanos",
        Schema::LocalTimestampMillis => "local-timestamp-millis",
        Schema::LocalTimestampMicros => "local-timestamp-micros",
        Schema::LocalTimestampNanos => "local-timestamp-nanos",
        Schema::Duration(_) => "duration",
        Schema::Ref { .. } => "reference",
    }
}

/// Whether the two schemas are the same Avro type. Named types must also
/// agree on their name, and fixed types on their width, because both change
/// how the bytes are read.
fn same_kind(writer: &Schema, projection: &Schema) -> bool {
    match (writer, projection) {
        (Schema::Enum(a), Schema::Enum(b)) => a.name == b.name && a.symbols == b.symbols,
        (Schema::Fixed(a), Schema::Fixed(b)) => a.name == b.name && a.size == b.size,
        (Schema::Record(a), Schema::Record(b)) => a.name == b.name,
        (Schema::Ref { name: a }, Schema::Ref { name: b }) => a == b,
        (Schema::Decimal(a), Schema::Decimal(b)) => {
            a.precision == b.precision
                && a.scale == b.scale
                && same_decimal_kind(&a.inner, &b.inner)
        }
        (Schema::Uuid(a), Schema::Uuid(b)) => same_uuid_kind(a, b),
        _ => kind_name(writer) == kind_name(projection),
    }
}

fn same_decimal_kind(writer: &InnerDecimalSchema, projection: &InnerDecimalSchema) -> bool {
    match (writer, projection) {
        (InnerDecimalSchema::Bytes, InnerDecimalSchema::Bytes) => true,
        (InnerDecimalSchema::Fixed(a), InnerDecimalSchema::Fixed(b)) => {
            a.name == b.name && a.size == b.size
        }
        _ => false,
    }
}

fn same_uuid_kind(writer: &UuidSchema, projection: &UuidSchema) -> bool {
    match (writer, projection) {
        (UuidSchema::Bytes, UuidSchema::Bytes) | (UuidSchema::String, UuidSchema::String) => true,
        (UuidSchema::Fixed(a), UuidSchema::Fixed(b)) => a.name == b.name && a.size == b.size,
        _ => false,
    }
}

/// Whether every value in this subtree can be built by `Context::read`
/// without apache-avro's help. False for decimals and named references.
fn self_contained(schema: &Schema) -> bool {
    match schema {
        Schema::Ref { .. } | Schema::Decimal(_) | Schema::BigDecimal => false,
        Schema::Record(record) => record.fields.iter().all(|f| self_contained(&f.schema)),
        Schema::Array(array) => self_contained(&array.items),
        Schema::Map(map) => self_contained(&map.types),
        Schema::Union(union) => union.variants().iter().all(self_contained),
        _ => true,
    }
}

/// Indexes the writer schema's named types so `Schema::Ref` can be resolved
/// while stepping over bytes. Inserting before recursing terminates on
/// recursive schemas.
fn collect_names(schema: &Schema, names: &mut HashMap<Name, Schema>) {
    match schema {
        Schema::Record(record) => {
            if names.insert(record.name.clone(), schema.clone()).is_none() {
                for field in &record.fields {
                    collect_names(&field.schema, names);
                }
            }
        }
        Schema::Enum(enumeration) => {
            names.insert(enumeration.name.clone(), schema.clone());
        }
        Schema::Fixed(fixed) => {
            names.insert(fixed.name.clone(), schema.clone());
        }
        Schema::Array(array) => collect_names(&array.items, names),
        Schema::Map(map) => collect_names(&map.types, names),
        Schema::Union(union) => {
            for variant in union.variants() {
                collect_names(variant, names);
            }
        }
        Schema::Decimal(decimal) => {
            if let InnerDecimalSchema::Fixed(fixed) = &decimal.inner {
                names.insert(fixed.name.clone(), schema.clone());
            }
        }
        Schema::Uuid(UuidSchema::Fixed(fixed)) | Schema::Duration(fixed) => {
            names.insert(fixed.name.clone(), schema.clone());
        }
        _ => {}
    }
}

// ---------------------------------------------------------------------------
// Byte-level primitives. Every read is bounds-checked against the containing
// buffer, which is what bounds allocation (see the module docs).
// ---------------------------------------------------------------------------

fn take<'a>(input: &mut &'a [u8], count: usize) -> Result<&'a [u8], String> {
    if count > input.len() {
        return Err(format!(
            "{TRUNCATED}: need {count} bytes, {} remain",
            input.len()
        ));
    }
    let (head, rest) = input.split_at(count);
    *input = rest;
    Ok(head)
}

/// Avro's zigzag variable-length integer.
fn read_long(input: &mut &[u8]) -> Result<i64, String> {
    let mut raw: u64 = 0;
    let mut shift = 0;
    loop {
        let (&byte, rest) = input.split_first().ok_or_else(|| TRUNCATED.to_string())?;
        *input = rest;
        raw |= u64::from(byte & 0x7f) << shift;
        if byte & 0x80 == 0 {
            // Zigzag: the low bit is the sign.
            return Ok(((raw >> 1) as i64) ^ -((raw & 1) as i64));
        }
        shift += 7;
        if shift > 63 {
            return Err("Avro varint longer than 64 bits".to_string());
        }
    }
}

/// Checks a length prefix against the bytes actually available. This is the
/// whole allocation bound: a length larger than the containing buffer is
/// malformed by definition, so no attacker-controlled length is ever
/// honored and no separate byte budget is needed.
fn check_len(length: i64, remaining: usize) -> Result<usize, String> {
    if length < 0 {
        return Err(format!("Negative Avro length prefix: {length}"));
    }
    let length = length as usize;
    if length > remaining {
        return Err(format!(
            "Avro length prefix {length} exceeds the {remaining} bytes remaining"
        ));
    }
    Ok(length)
}

fn read_bytes<'a>(input: &mut &'a [u8]) -> Result<&'a [u8], String> {
    let raw = read_long(input)?;
    let length = check_len(raw, input.len())?;
    take(input, length)
}

// ---------------------------------------------------------------------------
// Walking
// ---------------------------------------------------------------------------

struct Context<'a> {
    names: &'a HashMap<Name, Schema>,
    root: &'a Schema,
}

impl Context<'_> {
    fn resolve<'s>(&'s self, schema: &'s Schema) -> Result<&'s Schema, String> {
        let Schema::Ref { name } = schema else {
            return Ok(schema);
        };
        self.names.get(name).ok_or_else(|| {
            format!(
                "Unknown named type {} in writer schema",
                name.fullname(None)
            )
        })
    }

    /// Steps over one value, allocating nothing.
    fn skip(&self, schema: &Schema, input: &mut &[u8]) -> Result<(), String> {
        match self.resolve(schema)? {
            Schema::Null => Ok(()),
            Schema::Boolean => take(input, 1).map(|_| ()),
            Schema::Int
            | Schema::Long
            | Schema::Date
            | Schema::TimeMillis
            | Schema::TimeMicros
            | Schema::TimestampMillis
            | Schema::TimestampMicros
            | Schema::TimestampNanos
            | Schema::LocalTimestampMillis
            | Schema::LocalTimestampMicros
            | Schema::LocalTimestampNanos
            | Schema::Enum(_) => read_long(input).map(|_| ()),
            Schema::Float => take(input, 4).map(|_| ()),
            Schema::Double => take(input, 8).map(|_| ()),
            Schema::Duration(_) => take(input, 12).map(|_| ()),
            Schema::Bytes | Schema::String | Schema::BigDecimal => read_bytes(input).map(|_| ()),
            Schema::Fixed(fixed) => take(input, fixed.size).map(|_| ()),
            Schema::Decimal(decimal) => match &decimal.inner {
                InnerDecimalSchema::Bytes => read_bytes(input).map(|_| ()),
                InnerDecimalSchema::Fixed(fixed) => take(input, fixed.size).map(|_| ()),
            },
            Schema::Uuid(uuid) => match uuid {
                UuidSchema::Bytes | UuidSchema::String => read_bytes(input).map(|_| ()),
                UuidSchema::Fixed(fixed) => take(input, fixed.size).map(|_| ()),
            },
            Schema::Union(union) => {
                let branch = read_long(input)?;
                let variants = union.variants();
                let index = usize::try_from(branch)
                    .ok()
                    .filter(|index| *index < variants.len())
                    .ok_or_else(|| {
                        format!(
                            "Union branch {branch} out of range for {} variants",
                            variants.len()
                        )
                    })?;
                self.skip(&variants[index], input)
            }
            Schema::Record(record) => {
                for field in &record.fields {
                    self.skip(&field.schema, input)?;
                }
                Ok(())
            }
            Schema::Array(array) => {
                self.skip_blocks(input, |context, input| context.skip(&array.items, input))
            }
            Schema::Map(map) => self.skip_blocks(input, |context, input| {
                read_bytes(input)?;
                context.skip(&map.types, input)
            }),
            Schema::Ref { .. } => unreachable!("resolve() returns a concrete schema"),
        }
    }

    /// Walks the block structure shared by arrays and maps. A block with a
    /// negative count carries its byte size, which lets the whole block be
    /// stepped over with one seek -- this is where most of the projection
    /// win on Iceberg manifests comes from.
    fn skip_blocks(
        &self,
        input: &mut &[u8],
        skip_item: impl Fn(&Self, &mut &[u8]) -> Result<(), String>,
    ) -> Result<(), String> {
        loop {
            let count = read_long(input)?;
            if count == 0 {
                return Ok(());
            }
            if count < 0 {
                // Negative count means the block carries its byte size, so
                // the entire block is one seek. This is where most of the
                // projection win on Iceberg manifests comes from.
                let raw_size = read_long(input)?;
                let size = check_len(raw_size, input.len())?;
                take(input, size)?;
                continue;
            }
            for _ in 0..count {
                let before = input.len();
                skip_item(self, input)?;
                // Zero-width items (an array of nulls, say) consume nothing,
                // so the remaining count cannot advance the cursor either.
                // Stopping here keeps a corrupt count from spinning.
                if input.len() == before {
                    break;
                }
            }
        }
    }

    /// Decodes one value according to `plan`.
    fn read(&self, plan: &Plan, schema: &Schema, input: &mut &[u8]) -> Result<Value, String> {
        match plan {
            Plan::Drop => {
                self.skip(schema, input)?;
                Ok(Value::Null)
            }
            Plan::Delegate => from_avro_datum_schemata(schema, vec![self.root], input, None)
                .map_err(|error| error.to_string()),
            Plan::Take => self.read_all(schema, input),
            Plan::Record { fields, kept } => {
                let Schema::Record(record) = self.resolve(schema)? else {
                    return Err(format!("Expected a record, found {}", kind_name(schema)));
                };
                let mut out = Vec::with_capacity(*kept);
                for (field_plan, writer_field) in fields.iter().zip(&record.fields) {
                    match &field_plan.name {
                        Some(name) => {
                            let value = self.read(&field_plan.plan, &writer_field.schema, input)?;
                            out.push((name.clone(), value));
                        }
                        None => self.skip(&writer_field.schema, input)?,
                    }
                }
                Ok(Value::Record(out))
            }
            Plan::Array(item_plan) => {
                let Schema::Array(array) = self.resolve(schema)? else {
                    return Err(format!("Expected an array, found {}", kind_name(schema)));
                };
                let mut items = Vec::new();
                self.read_blocks(input, |context, input| {
                    items.push(context.read(item_plan, &array.items, input)?);
                    Ok(())
                })?;
                Ok(Value::Array(items))
            }
            Plan::Map(value_plan) => {
                let Schema::Map(map) = self.resolve(schema)? else {
                    return Err(format!("Expected a map, found {}", kind_name(schema)));
                };
                let mut entries = HashMap::new();
                self.read_blocks(input, |context, input| {
                    let key = utf8(read_bytes(input)?)?;
                    entries.insert(key, context.read(value_plan, &map.types, input)?);
                    Ok(())
                })?;
                Ok(Value::Map(entries))
            }
            Plan::Union(branch_plans) => {
                let Schema::Union(union) = self.resolve(schema)? else {
                    return Err(format!("Expected a union, found {}", kind_name(schema)));
                };
                let branch = read_long(input)?;
                let variants = union.variants();
                let index = usize::try_from(branch)
                    .ok()
                    .filter(|index| *index < variants.len())
                    .ok_or_else(|| {
                        format!(
                            "Union branch {branch} out of range for {} variants",
                            variants.len()
                        )
                    })?;
                let value = self.read(&branch_plans[index], &variants[index], input)?;
                Ok(Value::Union(index as u32, Box::new(value)))
            }
        }
    }

    /// Decodes a whole subtree, keeping everything. Mirrors apache-avro's
    /// leaf-to-`Value` mapping; `self_contained` guarantees the variants
    /// this does not handle never reach it.
    fn read_all(&self, schema: &Schema, input: &mut &[u8]) -> Result<Value, String> {
        match self.resolve(schema)? {
            Schema::Null => Ok(Value::Null),
            Schema::Boolean => match take(input, 1)?[0] {
                0 => Ok(Value::Boolean(false)),
                1 => Ok(Value::Boolean(true)),
                other => Err(format!("Invalid Avro boolean byte {other}")),
            },
            Schema::Int => Ok(Value::Int(read_int(input)?)),
            Schema::Long => Ok(Value::Long(read_long(input)?)),
            Schema::Date => Ok(Value::Date(read_int(input)?)),
            Schema::TimeMillis => Ok(Value::TimeMillis(read_int(input)?)),
            Schema::TimeMicros => Ok(Value::TimeMicros(read_long(input)?)),
            Schema::TimestampMillis => Ok(Value::TimestampMillis(read_long(input)?)),
            Schema::TimestampMicros => Ok(Value::TimestampMicros(read_long(input)?)),
            Schema::TimestampNanos => Ok(Value::TimestampNanos(read_long(input)?)),
            Schema::LocalTimestampMillis => Ok(Value::LocalTimestampMillis(read_long(input)?)),
            Schema::LocalTimestampMicros => Ok(Value::LocalTimestampMicros(read_long(input)?)),
            Schema::LocalTimestampNanos => Ok(Value::LocalTimestampNanos(read_long(input)?)),
            Schema::Float => {
                let bytes: [u8; 4] = take(input, 4)?.try_into().expect("checked width");
                Ok(Value::Float(f32::from_le_bytes(bytes)))
            }
            Schema::Double => {
                let bytes: [u8; 8] = take(input, 8)?.try_into().expect("checked width");
                Ok(Value::Double(f64::from_le_bytes(bytes)))
            }
            Schema::Duration(_) => {
                let bytes: [u8; 12] = take(input, 12)?.try_into().expect("checked width");
                Ok(Value::Duration(Duration::from(bytes)))
            }
            Schema::Bytes => Ok(Value::Bytes(read_bytes(input)?.to_vec())),
            Schema::String => Ok(Value::String(utf8(read_bytes(input)?)?)),
            Schema::Uuid(uuid_schema) => {
                // apache-avro accepts both the 16-byte form and the textual
                // form; mirror that rather than being stricter.
                let bytes = match uuid_schema {
                    UuidSchema::Bytes | UuidSchema::String => read_bytes(input)?,
                    UuidSchema::Fixed(fixed) => take(input, fixed.size)?,
                };
                let uuid = if bytes.len() == 16 {
                    Uuid::from_slice(bytes).map_err(|error| error.to_string())?
                } else {
                    Uuid::parse_str(&utf8(bytes)?).map_err(|error| error.to_string())?
                };
                Ok(Value::Uuid(uuid))
            }
            Schema::Fixed(fixed) => Ok(Value::Fixed(fixed.size, take(input, fixed.size)?.to_vec())),
            Schema::Enum(enumeration) => {
                let position = read_long(input)?;
                let symbol = usize::try_from(position)
                    .ok()
                    .and_then(|index| enumeration.symbols.get(index))
                    .ok_or_else(|| {
                        format!(
                            "Enum position {position} out of range for {} symbols",
                            enumeration.symbols.len()
                        )
                    })?;
                Ok(Value::Enum(position as u32, symbol.clone()))
            }
            Schema::Union(union) => {
                let branch = read_long(input)?;
                let variants = union.variants();
                let index = usize::try_from(branch)
                    .ok()
                    .filter(|index| *index < variants.len())
                    .ok_or_else(|| {
                        format!(
                            "Union branch {branch} out of range for {} variants",
                            variants.len()
                        )
                    })?;
                let value = self.read_all(&variants[index], input)?;
                Ok(Value::Union(index as u32, Box::new(value)))
            }
            Schema::Record(record) => {
                let mut out = Vec::with_capacity(record.fields.len());
                for field in &record.fields {
                    out.push((field.name.clone(), self.read_all(&field.schema, input)?));
                }
                Ok(Value::Record(out))
            }
            Schema::Array(array) => {
                let mut items = Vec::new();
                self.read_blocks(input, |context, input| {
                    items.push(context.read_all(&array.items, input)?);
                    Ok(())
                })?;
                Ok(Value::Array(items))
            }
            Schema::Map(map) => {
                let mut entries = HashMap::new();
                self.read_blocks(input, |context, input| {
                    let key = utf8(read_bytes(input)?)?;
                    entries.insert(key, context.read_all(&map.types, input)?);
                    Ok(())
                })?;
                Ok(Value::Map(entries))
            }
            other => Err(format!(
                "Internal error: {} should have been delegated to apache-avro",
                kind_name(other)
            )),
        }
    }

    /// Walks array/map blocks, calling `read_item` once per item. Unlike
    /// `skip_blocks` this cannot use a block's byte size to seek, because
    /// every item has to be produced.
    fn read_blocks(
        &self,
        input: &mut &[u8],
        mut read_item: impl FnMut(&Self, &mut &[u8]) -> Result<(), String>,
    ) -> Result<(), String> {
        loop {
            let count = read_long(input)?;
            if count == 0 {
                return Ok(());
            }
            let items = if count < 0 {
                let items = count.checked_neg().ok_or("Avro block count overflow")?;
                // The byte size is a hint for skipping; when reading we
                // still decode item by item, so it is read and discarded.
                read_long(input)?;
                items
            } else {
                count
            };
            if items > MAX_BLOCK_ITEMS {
                return Err(format!(
                    "Avro block claims {items} items, above the {MAX_BLOCK_ITEMS} cap"
                ));
            }
            for _ in 0..items {
                read_item(self, input)?;
            }
        }
    }
}

fn read_int(input: &mut &[u8]) -> Result<i32, String> {
    let value = read_long(input)?;
    i32::try_from(value).map_err(|_| format!("Avro int out of range: {value}"))
}

fn utf8(bytes: &[u8]) -> Result<String, String> {
    String::from_utf8(bytes.to_vec()).map_err(|_| "Avro string is not valid UTF-8".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;
    use apache_avro::{from_avro_datum, to_avro_datum};

    fn schema(json: &str) -> Schema {
        Schema::parse_str(json).expect("test schema should parse")
    }

    fn encode(writer: &Schema, value: Value) -> Vec<u8> {
        to_avro_datum(writer, value).expect("test value should encode")
    }

    /// The load-bearing check: decoding with an identity projection must
    /// produce exactly what apache-avro produces from the same bytes, and
    /// must consume exactly the same number of bytes. Any disagreement in
    /// this module's hand-written binary walk shows up here.
    fn assert_matches_upstream(json: &str, value: Value) {
        let writer = schema(json);
        let bytes = encode(&writer, value);

        let expected = from_avro_datum(&writer, &mut bytes.as_slice(), None)
            .expect("apache-avro should decode its own output");

        let projection = AvroProjection::compile(&writer, &writer)
            .unwrap_or_else(|error| panic!("identity projection for {json}: {error}"));
        let mut input = bytes.as_slice();
        let actual = projection
            .decode(&mut input)
            .unwrap_or_else(|error| panic!("projected decode for {json}: {error}"));

        assert_eq!(actual, expected, "value mismatch for {json}");
        assert!(
            input.is_empty(),
            "{} bytes left unconsumed for {json}",
            input.len()
        );
    }

    #[test]
    fn identity_projection_matches_upstream_for_scalars() {
        assert_matches_upstream(r#""null""#, Value::Null);
        assert_matches_upstream(r#""boolean""#, Value::Boolean(true));
        assert_matches_upstream(r#""boolean""#, Value::Boolean(false));
        assert_matches_upstream(r#""int""#, Value::Int(0));
        assert_matches_upstream(r#""int""#, Value::Int(-1));
        assert_matches_upstream(r#""int""#, Value::Int(i32::MIN));
        assert_matches_upstream(r#""int""#, Value::Int(i32::MAX));
        assert_matches_upstream(r#""long""#, Value::Long(i64::MIN));
        assert_matches_upstream(r#""long""#, Value::Long(i64::MAX));
        assert_matches_upstream(r#""float""#, Value::Float(-1.5));
        assert_matches_upstream(r#""double""#, Value::Double(f64::MAX));
        assert_matches_upstream(r#""bytes""#, Value::Bytes(vec![]));
        assert_matches_upstream(r#""bytes""#, Value::Bytes(vec![0, 1, 2, 255]));
        assert_matches_upstream(r#""string""#, Value::String(String::new()));
        assert_matches_upstream(r#""string""#, Value::String("hello \u{1f600}".into()));
    }

    #[test]
    fn identity_projection_matches_upstream_for_logical_types() {
        assert_matches_upstream(r#"{"type":"int","logicalType":"date"}"#, Value::Date(20630));
        assert_matches_upstream(
            r#"{"type":"int","logicalType":"time-millis"}"#,
            Value::TimeMillis(3_600_000),
        );
        assert_matches_upstream(
            r#"{"type":"long","logicalType":"time-micros"}"#,
            Value::TimeMicros(1),
        );
        assert_matches_upstream(
            r#"{"type":"long","logicalType":"timestamp-millis"}"#,
            Value::TimestampMillis(1_753_000_000_000),
        );
        assert_matches_upstream(
            r#"{"type":"long","logicalType":"timestamp-micros"}"#,
            Value::TimestampMicros(-1),
        );
        assert_matches_upstream(
            r#"{"type":"long","logicalType":"local-timestamp-millis"}"#,
            Value::LocalTimestampMillis(7),
        );
        assert_matches_upstream(
            r#"{"type":"string","logicalType":"uuid"}"#,
            Value::Uuid(Uuid::parse_str("8f3c1d92-4b7a-4e51-9c2f-6d0a1e77b3c4").unwrap()),
        );
        assert_matches_upstream(
            r#"{"type":"fixed","name":"D","size":12,"logicalType":"duration"}"#,
            Value::Duration(Duration::from([1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0])),
        );
    }

    #[test]
    fn identity_projection_matches_upstream_for_composites() {
        assert_matches_upstream(
            r#"{"type":"fixed","name":"F","size":4}"#,
            Value::Fixed(4, vec![9, 8, 7, 6]),
        );
        assert_matches_upstream(
            r#"{"type":"enum","name":"E","symbols":["A","B","C"]}"#,
            Value::Enum(2, "C".into()),
        );
        assert_matches_upstream(
            r#"{"type":"array","items":"long"}"#,
            Value::Array(vec![Value::Long(1), Value::Long(-2), Value::Long(3)]),
        );
        assert_matches_upstream(r#"{"type":"array","items":"long"}"#, Value::Array(vec![]));
        // An array of nulls: every item is zero-width, which is the case the
        // skip loop's progress check has to handle.
        assert_matches_upstream(
            r#"{"type":"array","items":"null"}"#,
            Value::Array(vec![Value::Null; 5]),
        );
        assert_matches_upstream(
            r#"{"type":"map","values":"int"}"#,
            Value::Map(HashMap::from([
                ("a".to_string(), Value::Int(1)),
                ("b".to_string(), Value::Int(2)),
            ])),
        );
        assert_matches_upstream(r#"["null","long"]"#, Value::Union(0, Box::new(Value::Null)));
        assert_matches_upstream(
            r#"["null","long"]"#,
            Value::Union(1, Box::new(Value::Long(42))),
        );
    }

    const NESTED: &str = r#"{"type":"record","name":"Outer","fields":[
        {"name":"id","type":"long"},
        {"name":"tags","type":{"type":"map","values":"string"}},
        {"name":"items","type":{"type":"array","items":
            {"type":"record","name":"Item","fields":[
                {"name":"key","type":"int"},
                {"name":"value","type":"bytes"}]}}},
        {"name":"maybe","type":["null","string"],"default":null}]}"#;

    fn nested_value() -> Value {
        Value::Record(vec![
            ("id".into(), Value::Long(7)),
            (
                "tags".into(),
                Value::Map(HashMap::from([(
                    "env".to_string(),
                    Value::String("prod".into()),
                )])),
            ),
            (
                "items".into(),
                Value::Array(vec![
                    Value::Record(vec![
                        ("key".into(), Value::Int(1)),
                        ("value".into(), Value::Bytes(vec![1, 2, 3])),
                    ]),
                    Value::Record(vec![
                        ("key".into(), Value::Int(2)),
                        ("value".into(), Value::Bytes(vec![])),
                    ]),
                ]),
            ),
            (
                "maybe".into(),
                Value::Union(1, Box::new(Value::String("x".into()))),
            ),
        ])
    }

    #[test]
    fn identity_projection_matches_upstream_for_nested_records() {
        assert_matches_upstream(NESTED, nested_value());
    }

    #[test]
    fn projection_drops_fields_and_consumes_every_byte() {
        let writer = schema(NESTED);
        let bytes = encode(&writer, nested_value());
        let projection_schema = schema(
            r#"{"type":"record","name":"Outer","fields":[
                {"name":"id","type":"long"},
                {"name":"maybe","type":["null","string"],"default":null}]}"#,
        );
        let projection = AvroProjection::compile(&writer, &projection_schema).unwrap();

        let mut input = bytes.as_slice();
        let value = projection.decode(&mut input).unwrap();

        assert_eq!(
            value,
            Value::Record(vec![
                ("id".into(), Value::Long(7)),
                (
                    "maybe".into(),
                    Value::Union(1, Box::new(Value::String("x".into())))
                ),
            ])
        );
        // The dropped map and array must still have been stepped over
        // exactly, or the next datum in a block would start at the wrong
        // offset.
        assert!(input.is_empty(), "{} bytes left unconsumed", input.len());
    }

    #[test]
    fn projection_can_narrow_records_inside_arrays() {
        let writer = schema(NESTED);
        let bytes = encode(&writer, nested_value());
        let projection = AvroProjection::compile(
            &writer,
            &schema(
                r#"{"type":"record","name":"Outer","fields":[
                    {"name":"items","type":{"type":"array","items":
                        {"type":"record","name":"Item","fields":[
                            {"name":"key","type":"int"}]}}}]}"#,
            ),
        )
        .unwrap();

        let mut input = bytes.as_slice();
        let value = projection.decode(&mut input).unwrap();
        assert_eq!(
            value,
            Value::Record(vec![(
                "items".into(),
                Value::Array(vec![
                    Value::Record(vec![("key".into(), Value::Int(1))]),
                    Value::Record(vec![("key".into(), Value::Int(2))]),
                ])
            )])
        );
        assert!(input.is_empty());
    }

    #[test]
    fn decimal_and_recursive_schemas_are_delegated_and_still_correct() {
        // Decimal's Value construction needs apache-avro internals, so the
        // plan delegates it; the result must still be identical.
        assert_matches_upstream(
            r#"{"type":"bytes","logicalType":"decimal","precision":9,"scale":2}"#,
            Value::Decimal(vec![0x04, 0xd2].into()),
        );

        // A self-referential schema reaches Schema::Ref, which is delegated.
        let recursive = r#"{"type":"record","name":"Node","fields":[
            {"name":"next","type":["null","Node"],"default":null}]}"#;
        assert_matches_upstream(
            recursive,
            Value::Record(vec![(
                "next".into(),
                Value::Union(
                    1,
                    Box::new(Value::Record(vec![(
                        "next".into(),
                        Value::Union(0, Box::new(Value::Null)),
                    )])),
                ),
            )]),
        );
    }

    /// Delegated subtrees are decoded whole by apache-avro, so a projection
    /// must not be allowed to narrow inside one: it would silently return
    /// more fields than were asked for.
    #[test]
    fn projection_cannot_narrow_inside_a_delegated_subtree() {
        let recursive = r#"{"type":"record","name":"Node","fields":[
            {"name":"payload","type":"long"},
            {"name":"next","type":["null","Node"],"default":null}]}"#;
        let writer = schema(recursive);

        // Asking for the whole record is fine: it delegates and returns
        // everything, which is exactly what was requested.
        let identity = AvroProjection::compile(&writer, &writer).unwrap();
        let bytes = encode(
            &writer,
            Value::Record(vec![
                ("payload".into(), Value::Long(5)),
                ("next".into(), Value::Union(0, Box::new(Value::Null))),
            ]),
        );
        let decoded = identity.decode(&mut bytes.as_slice()).unwrap();
        let Value::Record(fields) = &decoded else {
            panic!("expected a record")
        };
        assert_eq!(fields.len(), 2);

        // Asking for one of its two fields must be refused rather than
        // quietly handing back both.
        let narrowed = schema(
            r#"{"type":"record","name":"Node","fields":[{"name":"payload","type":"long"}]}"#,
        );
        let error = AvroProjection::compile(&writer, &narrowed).unwrap_err();
        assert!(
            error.contains("named type reference") || error.contains("decimal"),
            "unexpected error: {error}"
        );
    }

    #[test]
    fn projection_rejects_shapes_that_are_not_subsets() {
        let writer = schema(NESTED);

        let unknown_field =
            schema(r#"{"type":"record","name":"Outer","fields":[{"name":"nope","type":"long"}]}"#);
        let error = AvroProjection::compile(&writer, &unknown_field).unwrap_err();
        assert!(error.contains("nope"), "unexpected error: {error}");

        let wrong_type =
            schema(r#"{"type":"record","name":"Outer","fields":[{"name":"id","type":"string"}]}"#);
        let error = AvroProjection::compile(&writer, &wrong_type).unwrap_err();
        assert!(error.contains("id"), "unexpected error: {error}");

        let wrong_name =
            schema(r#"{"type":"record","name":"Other","fields":[{"name":"id","type":"long"}]}"#);
        assert!(AvroProjection::compile(&writer, &wrong_name).is_err());

        // Union branch indices are encoded in the data, so narrowing a union
        // would change what the encoded index means.
        let narrowed_union = schema(
            r#"{"type":"record","name":"Outer","fields":[
                {"name":"maybe","type":["null"],"default":null}]}"#,
        );
        let error = AvroProjection::compile(&writer, &narrowed_union).unwrap_err();
        assert!(
            error.contains("union branches"),
            "unexpected error: {error}"
        );
    }

    #[test]
    fn malformed_input_is_rejected_rather_than_allocated_for() {
        let writer = schema(r#""bytes""#);
        let projection = AvroProjection::compile(&writer, &writer).unwrap();

        // A length prefix of 2^40 with two bytes of payload: the bound is
        // the containing buffer, so this must fail without allocating.
        let mut hostile: Vec<u8> = Vec::new();
        let mut length = (1u64 << 40) << 1; // zigzag-encode a positive length
        while length >= 0x80 {
            hostile.push((length as u8 & 0x7f) | 0x80);
            length >>= 7;
        }
        hostile.push(length as u8);
        hostile.extend_from_slice(&[0xaa, 0xbb]);

        let error = projection.decode(&mut hostile.as_slice()).unwrap_err();
        assert!(error.contains("exceeds"), "unexpected error: {error}");
    }

    #[test]
    fn truncated_input_is_rejected() {
        let writer = schema(NESTED);
        let bytes = encode(&writer, nested_value());
        let projection = AvroProjection::compile(&writer, &writer).unwrap();
        for cut in 1..bytes.len() {
            let mut truncated = &bytes[..cut];
            assert!(
                projection.decode(&mut truncated).is_err(),
                "decode should fail on input truncated to {cut} bytes"
            );
        }
    }

    #[test]
    fn varint_longer_than_64_bits_is_rejected() {
        let writer = schema(r#""long""#);
        let projection = AvroProjection::compile(&writer, &writer).unwrap();
        let runaway = vec![0xffu8; 12];
        assert!(projection.decode(&mut runaway.as_slice()).is_err());
    }
}
