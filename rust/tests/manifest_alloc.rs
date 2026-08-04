//! Counts heap allocations per Iceberg manifest entry decode, and shows how
//! that count scales with the width of the table the manifest describes.
//!
//! Why this exists: benchmarks/manifest_probe.cc measures *time* and shows
//! this binding decoding a v2 manifest ~1.6x slower than avro-cpp, growing to
//! ~2.7x once the caller reads the metrics back out. Time alone does not say
//! where it goes. This test says: apache-avro represents a record as
//! `Vec<(String, Value)>`, and Iceberg encodes `map<int, long>` metrics as
//! `array<record{key, value}>` (Avro map keys must be strings), so every
//! column contributes six two-field sub-records to every entry -- and each of
//! those heap-allocates the literal field names "key" and "value" again.
//!
//! The marginal cost per column printed below is what a decoder that did not
//! build a Value tree would not have to pay.
//!
//! This is a measurement, so it prints a table. Run it with:
//!   cargo test --test manifest_alloc -- --nocapture
//! The assertions guard only the structural claims the design rests on, not
//! the exact counts.

use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::Mutex;
use std::sync::atomic::{AtomicUsize, Ordering};

/// The counting allocator below is process-global, so two tests measuring at
/// once would each see the other's allocations. Every test in this file takes
/// this lock for its whole body, not just around `measure`: an unmeasured
/// allocation on another thread still moves the shared counters.
static MEASURING: Mutex<()> = Mutex::new(());

/// Ignores poisoning: a panicking test has already failed, and the counters
/// are reset by the next `measure` anyway.
fn measuring_lock() -> std::sync::MutexGuard<'static, ()> {
    MEASURING.lock().unwrap_or_else(|err| err.into_inner())
}

use rust::container::{AvroCodec, DataFileReader, DataFileWriter};
use rust::datum::{AvroDatumReader, decode_datum, decode_datum_resolved, encode_datum};
use rust::decode::AvroProjection;
use rust::schema::AvroSchema;
use rust::value::AvroValue;

// ---------------------------------------------------------------------------
// Counting allocator. Integration tests are their own crate, so the `unsafe`
// a GlobalAlloc impl requires does not touch the library's forbid(unsafe_code).
// ---------------------------------------------------------------------------

static OPERATIONS: AtomicUsize = AtomicUsize::new(0);
static BYTES: AtomicUsize = AtomicUsize::new(0);

struct Counting;

// Counts allocator *operations*: a realloc is counted because Vec growth is
// exactly the cost being measured, and ignoring it would flatter the result.
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        OPERATIONS.fetch_add(1, Ordering::Relaxed);
        BYTES.fetch_add(layout.size(), Ordering::Relaxed);
        unsafe { System.alloc(layout) }
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        OPERATIONS.fetch_add(1, Ordering::Relaxed);
        BYTES.fetch_add(layout.size(), Ordering::Relaxed);
        unsafe { System.alloc_zeroed(layout) }
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        OPERATIONS.fetch_add(1, Ordering::Relaxed);
        BYTES.fetch_add(new_size.saturating_sub(layout.size()), Ordering::Relaxed);
        unsafe { System.realloc(ptr, layout, new_size) }
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        unsafe { System.dealloc(ptr, layout) }
    }
}

#[global_allocator]
static ALLOCATOR: Counting = Counting;

#[derive(Clone, Copy)]
struct Cost {
    operations: usize,
    bytes: usize,
}

/// Runs `body` and reports what it allocated. The result is dropped inside
/// the measured region's caller, not inside it, so frees are not counted.
fn measure<T>(body: impl FnOnce() -> T) -> (T, Cost) {
    OPERATIONS.store(0, Ordering::Relaxed);
    BYTES.store(0, Ordering::Relaxed);
    let out = body();
    let cost = Cost {
        operations: OPERATIONS.load(Ordering::Relaxed),
        bytes: BYTES.load(Ordering::Relaxed),
    };
    (out, cost)
}

// ---------------------------------------------------------------------------
// The Iceberg v2 manifest_entry shape. Field ids are metadata and do not
// affect the binary layout, so they are omitted (see manifest_probe.cc).
// ---------------------------------------------------------------------------

const MANIFEST_JSON: &str = r#"{
"type":"record","name":"manifest_entry","fields":[
 {"name":"status","type":"int"},
 {"name":"snapshot_id","type":["null","long"],"default":null},
 {"name":"sequence_number","type":["null","long"],"default":null},
 {"name":"file_sequence_number","type":["null","long"],"default":null},
 {"name":"data_file","type":{"type":"record","name":"r2","fields":[
  {"name":"content","type":"int"},
  {"name":"file_path","type":"string"},
  {"name":"file_format","type":"string"},
  {"name":"partition","type":{"type":"record","name":"r102","fields":[
   {"name":"dept","type":["null","string"],"default":null},
   {"name":"event_day","type":["null","int"],"default":null}]}},
  {"name":"record_count","type":"long"},
  {"name":"file_size_in_bytes","type":"long"},
  {"name":"column_sizes","type":["null",{"type":"array","logicalType":"map",
   "items":{"type":"record","name":"k117_v118","fields":[
    {"name":"key","type":"int"},{"name":"value","type":"long"}]}}],"default":null},
  {"name":"value_counts","type":["null",{"type":"array","logicalType":"map",
   "items":{"type":"record","name":"k119_v120","fields":[
    {"name":"key","type":"int"},{"name":"value","type":"long"}]}}],"default":null},
  {"name":"null_value_counts","type":["null",{"type":"array","logicalType":"map",
   "items":{"type":"record","name":"k121_v122","fields":[
    {"name":"key","type":"int"},{"name":"value","type":"long"}]}}],"default":null},
  {"name":"nan_value_counts","type":["null",{"type":"array","logicalType":"map",
   "items":{"type":"record","name":"k138_v139","fields":[
    {"name":"key","type":"int"},{"name":"value","type":"long"}]}}],"default":null},
  {"name":"lower_bounds","type":["null",{"type":"array","logicalType":"map",
   "items":{"type":"record","name":"k126_v127","fields":[
    {"name":"key","type":"int"},{"name":"value","type":"bytes"}]}}],"default":null},
  {"name":"upper_bounds","type":["null",{"type":"array","logicalType":"map",
   "items":{"type":"record","name":"k129_v130","fields":[
    {"name":"key","type":"int"},{"name":"value","type":"bytes"}]}}],"default":null},
  {"name":"key_metadata","type":["null","bytes"],"default":null},
  {"name":"split_offsets","type":["null",{"type":"array","items":"long"}],"default":null},
  {"name":"equality_ids","type":["null",{"type":"array","items":"int"}],"default":null},
  {"name":"sort_order_id","type":["null","int"],"default":null}]}}]}"#;

/// What a query planner needs to list and size files: no metrics at all.
const PROJECTION_JSON: &str = r#"{
"type":"record","name":"manifest_entry","fields":[
 {"name":"status","type":"int"},
 {"name":"data_file","type":{"type":"record","name":"r2","fields":[
  {"name":"file_path","type":"string"},
  {"name":"record_count","type":"long"},
  {"name":"file_size_in_bytes","type":"long"}]}}]}"#;

const FILE_PATH: &[u8] = b"s3://warehouse-prod-us-east-1/lakehouse/analytics/events/data/\
dept=engineering/event_day=2026-07-14/00000-17-8f3c1d92-4b7a-4e51-9c2f-6d0a1e77b3c4-00001.parquet";

fn optional(value: AvroValue) -> AvroValue {
    AvroValue::create_union(1, &value)
}

fn metrics_map(columns: i32, bytes_value: bool) -> AvroValue {
    let mut array = AvroValue::create_array();
    for column in 0..columns {
        let mut pair = AvroValue::create_record();
        pair.record_put(b"key", &AvroValue::create_int(column))
            .unwrap();
        let value = if bytes_value {
            AvroValue::create_bytes(&[0x5a; 16])
        } else {
            AvroValue::create_long(i64::from(column) * 31)
        };
        pair.record_put(b"value", &value).unwrap();
        array.array_push(&pair).unwrap();
    }
    optional(array)
}

fn manifest_entry(columns: i32) -> AvroValue {
    let mut partition = AvroValue::create_record();
    partition
        .record_put(
            b"dept",
            &optional(AvroValue::create_string(b"engineering").unwrap()),
        )
        .unwrap();
    partition
        .record_put(b"event_day", &optional(AvroValue::create_int(20630)))
        .unwrap();

    let mut offsets = AvroValue::create_array();
    for split in 0..8i64 {
        offsets
            .array_push(&AvroValue::create_long(4 + split * 6553600))
            .unwrap();
    }

    let mut file = AvroValue::create_record();
    file.record_put(b"content", &AvroValue::create_int(0))
        .unwrap();
    file.record_put(b"file_path", &AvroValue::create_string(FILE_PATH).unwrap())
        .unwrap();
    file.record_put(
        b"file_format",
        &AvroValue::create_string(b"parquet").unwrap(),
    )
    .unwrap();
    file.record_put(b"partition", &partition).unwrap();
    file.record_put(b"record_count", &AvroValue::create_long(100_017))
        .unwrap();
    file.record_put(b"file_size_in_bytes", &AvroValue::create_long(52_428_800))
        .unwrap();
    file.record_put(b"column_sizes", &metrics_map(columns, false))
        .unwrap();
    file.record_put(b"value_counts", &metrics_map(columns, false))
        .unwrap();
    file.record_put(b"null_value_counts", &metrics_map(columns, false))
        .unwrap();
    file.record_put(b"nan_value_counts", &metrics_map(columns, false))
        .unwrap();
    file.record_put(b"lower_bounds", &metrics_map(columns, true))
        .unwrap();
    file.record_put(b"upper_bounds", &metrics_map(columns, true))
        .unwrap();
    file.record_put(
        b"key_metadata",
        &AvroValue::create_union(0, &AvroValue::create_null()),
    )
    .unwrap();
    file.record_put(b"split_offsets", &optional(offsets))
        .unwrap();
    file.record_put(
        b"equality_ids",
        &AvroValue::create_union(0, &AvroValue::create_null()),
    )
    .unwrap();
    file.record_put(b"sort_order_id", &optional(AvroValue::create_int(0)))
        .unwrap();

    let mut entry = AvroValue::create_record();
    entry
        .record_put(b"status", &AvroValue::create_int(1))
        .unwrap();
    entry
        .record_put(
            b"snapshot_id",
            &optional(AvroValue::create_long(7_856_392_845_000_000_000)),
        )
        .unwrap();
    entry
        .record_put(b"sequence_number", &optional(AvroValue::create_long(33)))
        .unwrap();
    entry
        .record_put(
            b"file_sequence_number",
            &optional(AvroValue::create_long(33)),
        )
        .unwrap();
    entry.record_put(b"data_file", &file).unwrap();
    entry
}

#[test]
fn manifest_decode_allocation_profile() {
    let _measuring = measuring_lock();
    let schema = AvroSchema::parse(MANIFEST_JSON.as_bytes()).unwrap();
    let projection = AvroSchema::parse(PROJECTION_JSON.as_bytes()).unwrap();

    let widths = [0i32, 5, 10, 20, 40, 80];
    // (columns, payload bytes, full decode, reader-schema resolution,
    //  byte-level projection)
    let mut profile: Vec<(i32, usize, Cost, Cost, Cost)> = Vec::new();

    // Compiled once, as DataFileReader::with_projection does per file. The
    // measured region is the per-datum decode, which is what scales.
    let compiled = AvroProjection::create(&schema, &projection).unwrap();

    for columns in widths {
        let encoded = encode_datum(&schema, &manifest_entry(columns)).unwrap();
        let payload = encoded.as_slice().to_vec();

        // Warm any lazily-initialized state so it is not billed to the first
        // measured decode.
        decode_datum(&schema, &payload).unwrap();
        decode_datum_resolved(&schema, &projection, &payload).unwrap();
        compiled.decode_datum(&payload).unwrap();

        let (full, full_cost) = measure(|| decode_datum(&schema, &payload).unwrap());
        let (resolved, resolved_cost) =
            measure(|| decode_datum_resolved(&schema, &projection, &payload).unwrap());
        let (projected, projected_cost) = measure(|| compiled.decode_datum(&payload).unwrap());
        drop(full);
        drop(resolved);
        drop(projected);

        profile.push((
            columns,
            payload.len(),
            full_cost,
            resolved_cost,
            projected_cost,
        ));
    }

    println!();
    println!("Heap allocations to decode ONE Iceberg v2 manifest entry");
    println!("(apache-avro via this binding; payload is the encoded datum)");
    println!();
    println!(
        "{:>7}  {:>8}  {:>9}  {:>10}  {:>9}  {:>10}  {:>9}  {:>10}",
        "columns",
        "payload",
        "full ops",
        "full bytes",
        "resv ops",
        "resv bytes",
        "proj ops",
        "proj bytes"
    );
    for (columns, payload, full, resolved, projected) in &profile {
        println!(
            "{:>7}  {:>8}  {:>9}  {:>10}  {:>9}  {:>10}  {:>9}  {:>10}",
            columns,
            payload,
            full.operations,
            full.bytes,
            resolved.operations,
            resolved.bytes,
            projected.operations,
            projected.bytes
        );
    }

    let baseline = profile[0].2.operations;
    let widest = profile.last().unwrap();
    let per_column = (widest.2.operations - baseline) as f64 / f64::from(widest.0);
    let projected_per_column =
        (widest.4.operations as f64 - profile[0].4.operations as f64) / f64::from(widest.0);
    let twenty = profile.iter().find(|row| row.0 == 20).unwrap();
    println!();
    println!("full decode, fixed cost (0 columns):  {baseline} allocator operations");
    println!("full decode, marginal per column:     {per_column:.1} operations");
    println!("projected,   marginal per column:     {projected_per_column:.1} operations");
    println!(
        "a 20-column table: {} ops full, {} resolved, {} projected",
        twenty.2.operations, twenty.3.operations, twenty.4.operations
    );

    // The first call builds the caller-owned tree and initializes Apache's
    // reusable datum reader. Measure the second same-shaped entry, which is
    // the steady state of DataFileReader::next_value_into.
    let entry = manifest_entry(20);
    let mut writer = DataFileWriter::create(&schema, AvroCodec::Null).unwrap();
    writer.append(&entry).unwrap();
    writer.append(&entry).unwrap();
    let file = writer.finish().unwrap();
    let mut reader = DataFileReader::from_bytes(file.as_slice()).unwrap();
    let mut reused = AvroValue::default();
    assert!(reader.next_value_into(&mut reused).unwrap());
    let (decoded, reusable_cost) = measure(|| reader.next_value_into(&mut reused).unwrap());
    assert!(decoded);
    println!(
        "a 20-column reusable steady-state entry: {} ops, {} bytes",
        reusable_cost.operations, reusable_cost.bytes
    );
    println!();

    // Claim 1: allocation count is driven by table width, not by a fixed
    // per-entry overhead. This is what makes wide tables disproportionately
    // expensive and what a non-materializing decoder would avoid.
    assert!(
        per_column >= 6.0,
        "expected at least one allocation per metrics map per column \
         (6 maps), got {per_column:.1}"
    );

    // Claim 2: a projected *reader schema* does NOT reduce allocations.
    // apache-avro materializes the full writer-schema value and then
    // resolves, so schema resolution cannot skip work at the byte level the
    // way avro-cpp's ResolvingDecoder does. This is why the binding needed
    // its own projection rather than reusing decode_datum_resolved.
    let widest_full = widest.2.operations;
    let widest_resolved = widest.3.operations;
    assert!(
        widest_resolved as f64 > widest_full as f64 * 0.5,
        "resolved decode allocated {widest_resolved} ops vs {widest_full} full; \
         if this now drops sharply, apache-avro gained byte-level projection \
         and crate::decode may no longer be needed"
    );

    // Claim 3: byte-level projection (crate::decode) makes allocation a
    // function of the fields actually read rather than of table width. This
    // is the success criterion from doc/specs/AvroTokenStream.md.
    assert!(
        projected_per_column < 1.0,
        "projected decode still costs {projected_per_column:.1} allocations per \
         column; it should be flat in table width"
    );
    assert!(
        widest.4.operations * 10 < widest_full,
        "projected decode allocated {} ops against {widest_full} for a full \
         decode; expected an order of magnitude less",
        widest.4.operations
    );

    assert!(
        reusable_cost.operations * 100 < twenty.2.operations,
        "reusable decode allocated {} ops against {} for an owned decode",
        reusable_cost.operations,
        twenty.2.operations
    );
}

/// The bare-datum counterpart to the container reader's reuse claim, and
/// success criterion 3 of doc/specs/AvroDatumReader.md: once the value tree
/// has the right shape, a same-shaped datum decodes without touching the
/// allocator at all.
#[test]
fn datum_reader_reuse_removes_steady_state_allocation() {
    let _measuring = measuring_lock();
    let schema = AvroSchema::parse(MANIFEST_JSON.as_bytes()).unwrap();
    let entry = manifest_entry(20);
    let encoded = encode_datum(&schema, &entry).unwrap();
    let bytes = encoded.as_slice();

    let (_, owned_cost) = measure(|| decode_datum(&schema, bytes).unwrap());

    let reader = AvroDatumReader::create(&schema).unwrap();
    let mut value = AvroValue::default();
    // First call builds the tree; only the steady state is the claim.
    reader.decode_into(bytes, &mut value).unwrap();
    let (_, reuse_cost) = measure(|| reader.decode_into(bytes, &mut value).unwrap());

    println!(
        "20-column bare datum: owned {} ops / {} bytes, reused {} ops / {} bytes",
        owned_cost.operations, owned_cost.bytes, reuse_cost.operations, reuse_cost.bytes
    );

    assert_eq!(
        reuse_cost.operations, 0,
        "expected no allocator operations for a same-shaped datum, got {}",
        reuse_cost.operations
    );
    assert_eq!(reuse_cost.bytes, 0);
    assert!(
        owned_cost.operations > 0,
        "the owned decode should allocate; otherwise this test proves nothing"
    );
}
