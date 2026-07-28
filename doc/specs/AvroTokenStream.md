# Spec: Non-materializing Avro decode (projection, then token cursor)

Status: Stage 1 APPROVED (GO 2026-07-28), implemented. Stage 2 not started.

## Purpose & user problem

Consumers decode large Apache Iceberg v2 manifests (150 MB and up) through
this binding and report it running roughly 10x slower than an equivalent
avro-cpp implementation. The reported diagnosis was that the AST-based
`Value` deserialization is the cause, through heap allocation and enum
inflation, and that the fix is a zero-copy or SAX-style token API that
bypasses the `Value` builder entirely.

The initial measurement below (benchmarks/manifest_probe.cc,
rust/tests/manifest_alloc.rs, both added for this spec) confirms the 10x and
separates allocation, projection, and read-back costs. A follow-up reusable
decode row materially revises the first interpretation: rebuilding and
destroying a `Value` tree is a major cost, although it is still not enough by
itself to explain an order-of-magnitude end-to-end gap.

### Why manifests specifically

Iceberg's per-column metrics are typed `map<int, long>` and
`map<int, binary>`. Avro map keys must be strings, so Iceberg encodes them as
`array<record{key, value}>` with a `"logicalType": "map"` annotation
(confirmed against `pyiceberg/utils/schema_conversion.py`). A table with N
columns therefore carries **6N two-field sub-records inside every manifest
entry** (`column_sizes`, `value_counts`, `null_value_counts`,
`nan_value_counts`, `lower_bounds`, `upper_bounds`).

apache-avro represents a record as `Vec<(String, Value)>`. Every one of those
6N sub-records heap-allocates the literal field names `"key"` and `"value"`
again, per entry. avro-cpp's `GenericRecord` keeps field names in the shared
schema node and stores only values, so it allocates nothing per field name.

## Measurement

Hardware: this repo's dev host, aarch64, single-threaded, all IO in memory.
Both libraries read the same avrocpp-written bytes, and startup asserts the
two sides compute identical checksums, so neither can be doing less work.

Per entry, ours/avrocpp, 2000 entries, null codec:

| cols | B/entry | decode_only | decode_full | project_decode | read-back only | realistic |
|-----:|--------:|------------:|------------:|---------------:|---------------:|----------:|
|    5 |     506 |       2.28x |       3.54x |          3.53x |          28.7x |     4.48x |
|   20 |    1226 |       1.65x |       2.74x |          4.22x |          23.7x |     6.65x |
|   80 |    4209 |       1.40x |       2.48x |          4.50x |          30.9x |     7.98x |
|  160 |    8531 |       1.30x |       2.34x |          4.83x |          30.4x |     8.72x |

- `decode_only` - decode the entry, touch nothing. The AST build alone.
- `decode_full` - decode, then read every metrics leaf back out (via
  `AvroPath`, already the fast accessor).
- `project_decode` - decode through a projected *reader* schema keeping only
  `status`, `file_path`, `record_count`, `file_size_in_bytes`.
- `read-back only` - `decode_full` minus `decode_only`, both sides.
- `realistic` - our `decode_full` against avrocpp's `project_decode`: a
  consumer that must materialize everything, against one that projects. This
  is the comparison that reproduces the reported 10x.

Ratios are stable from 2.4 MB to 167 MB (`MANIFEST_ENTRIES=128247`), so this
is per-entry CPU cost, not a cache cliff.

Allocation profile, one 20-column entry (rust/tests/manifest_alloc.rs):

- **464 allocator operations** and **32.4 KB of heap** for a **1212-byte**
  payload: 27x byte amplification.
- Fixed cost 58 operations; **marginal cost ~20 operations per column**.
- A projected reader schema costs *more*, not less: **477** operations.
- After warming Apache's reusable datum reader and caller-owned tree, decode of
  the next same-shaped entry costs **0 allocator operations and 0 bytes**.

### What the numbers say

Three conclusions, in order of how much they change the design:

1. **Rebuilding the AST is a major problem; having an AST is not.** The
   original `decode_only` row is 1.30x-2.28x behind, but it returns and drops
   a new owned `Value` per entry while avro-cpp overwrites one
   `GenericDatum`. `NextValueInto` makes the ownership equal. At 20 columns it
   improves 76.75k to 377.2k entries/s and is **3.46x faster than avro-cpp's
   109.09k**. This is a 4.91x win from reuse, not an order of magnitude, and
   it still decodes every metrics byte into a complete tree.

2. **Projection is the largest structural win.** avrocpp's `ResolvingDecoder`
   skips unread fields at the byte level; its `project_decode` is 2.4x-3.6x
   faster than its own `decode_only`. apache-avro materializes the full
   writer-schema value and *then* resolves, so our projected read is never
   faster than our unprojected one (4.22x behind at 20 columns, 4.83x at 160,
   and widening). The allocation test confirms the mechanism: 477 operations
   projected against 464 unprojected. This is a measured ceiling, not an
   estimate - avrocpp demonstrates it on the same bytes.

3. **Reading values back out costs 24-31x**, and that is *with* `AvroPath`,
   the accessor added specifically to avoid clone-on-read. `AvroValue::at`
   (rust/value.rs:873) re-walks from the root per leaf and does a linear
   `fields.iter().find()` string comparison at each record step. For a
   160-column entry that is ~1920 leaf reads per entry, each rescanning a
   16-field record. Paths fixed the copying; they did not make sequential
   traversal cheap.

### Follow-up: caller-owned reusable value

`DataFileReader::NextValueInto(AvroValue*)` overwrites compatible records,
arrays, unions, strings, bytes, fixed values and enums. It is the same
ownership contract as avro-cpp's `reader->read(datum)`: the caller consumes
the value before the next read.

The reusable binary walk is implemented by apache-avro's `Reader::read_into`,
`GenericDatumReader::read_value_into`, and `OwnedGenericDatumReader`. Orva
stores one owned datum reader beside its incremental container state and
only adapts block slices and error types; it does not maintain a duplicate
full-schema in-place decoder. Orva's byte-level projection decoder remains
separate because projection is not part of this upstream patch.

Same-process 20-column medians, 2000 entries, null codec, seven repetitions:

| work | owned Rust | reusable Rust | avrocpp | reusable vs avrocpp |
|------|-----------:|--------------:|--------:|---------------------:|
| decode only | 76.75k/s | **377.2k/s** | 109.09k/s | **3.46x faster** |
| planner walk | 68.78k/s | **257.0k/s** | 108.66k/s | **2.37x faster** |
| full metrics walk | 41.18k/s | **73.69k/s** | 104.53k/s | **1.42x slower** |

The planner row shows that reuse survives the real C++ bridge and normal
field access. The full-walk row isolates what remains: roughly 120
key/value subrecords at 20 columns are read through repeated `AvroPath`
calls. Reuse removes construction/destruction but does not provide a
sequential view over an existing tree.

The allocator test also isolates the mechanism from timing: the second
same-shaped 20-column entry decoded into one caller-owned value performs
**0 heap allocations**, versus 464 operations and 32,350 bytes for the
ordinary owned decode. Shape changes and the first value can still allocate.

Projection and reuse are complementary. At 20 columns native projection is
still about 466.7k entries/s because it skips all metrics bytes; reusable
full decode is 377.2k decode-only or 257.0k with planner access.

## Outcome (stage 1, implemented)

`crate::decode` (rust/decode.rs) compiles a projection against the writer
schema once and walks the Avro binary encoding, materializing only wanted
subtrees and seeking past the rest. Exposed as
`DataFileReader::CreateWithProjection` / `FromBytesWithProjection` /
`FromPathWithProjection`, `DataFileStreamReader::CreateWithProjection`, and
the reusable `AvroProjection` class for bare datums.

Measured by the `ours/project_native` row of benchmarks/manifest_probe.cc,
against the same planner fields as the rows above:

| cols | vs our decode_full | vs our old project_decode | vs avrocpp project_decode |
|-----:|-------------------:|--------------------------:|--------------------------:|
|    5 |              9.4x  |                     7.6x  |          **2.08x faster** |
|   20 |             12.2x  |                     7.8x  |          **1.87x faster** |
|   80 |             14.0x  |                     7.8x  |          **1.71x faster** |
|  160 |             14.2x  |                     7.6x  |          **1.56x faster** |

At the 150 MB scale (`MANIFEST_ENTRIES=128247`, 167 MB, 20 columns): 275 ms
against avrocpp's 501 ms and our own 3369 ms unprojected, i.e. ~607 MB/s.

Allocations per 20-column entry (rust/tests/manifest_alloc.rs), with the
projection compiled once as a real caller would:

| path | allocator ops | bytes |
|------|--------------:|------:|
| full decode | 464 | 32350 |
| reader-schema resolution | 477 | 36496 |
| **byte-level projection** | **8** | **613** |

84x fewer allocations, and **flat in table width**: 8 operations whether the
table has 0 or 80 columns, against a marginal ~26 per column before. That was
the actual success criterion, and it is the reason the time ratios above
*improve* rather than decay as tables get wider.

Both success criteria were exceeded: the target was "within ~1.5x of avrocpp
rather than ~6.7x behind", and the result is faster than avrocpp on every
width measured. Note this is a like-for-like comparison -- avro-cpp's
ResolvingDecoder is doing real byte-level skipping too; we come out ahead
because `GenericDatum` still allocates per field for what it does keep.

### What was built, and what it did not cost

The reversal is narrower than the spec anticipated. `crate::decode` owns only
the binary *walk*: zigzag varints, blocked arrays and maps, union dispatch,
length prefixes, fixed widths. It does not own value construction for
decimals, big-decimals, or named references -- `Plan::Delegate` hands those
subtrees to apache-avro, decided once at compile time, so a projection that
avoids them never pays for them. `#![forbid(unsafe_code)]` is intact.

Correctness rests on rust/decode.rs's `assert_matches_upstream`: for every
type the module handles, an identity projection must produce exactly what
`from_avro_datum` produces from the same bytes *and* consume exactly the same
number of bytes. Container-level tests then read every value of a multi-block
file, since a skip that miscounts one byte puts the next datum at the wrong
offset.

An interesting finding not in the original measurement:
`from_avro_datum_reader_schemata` rebuilds a `ResolvedSchema` (a full names
map) on *every call*, so the unprojected path pays that per datum. The
projected path builds it once at compile time, which is part of the win.

## Success criteria

- A 20-column, 150 MB manifest scanned for planner fields
  (`file_path`, `record_count`, `file_size_in_bytes`, plus the bounds of one
  predicate column) decodes at least 4x faster than today, putting us within
  ~1.5x of avrocpp's projected read rather than ~6.7x behind.
- Allocation count per entry becomes a function of *fields actually read*,
  not of table width. The `~26 operations per column` marginal cost goes to
  roughly zero for unread columns.
- Peak memory stays bounded as it is today: the streaming reader's guarantees
  (doc/specs/AvroStreamingIO.md) are not weakened.
- No `unsafe` in the library crate: `#![forbid(unsafe_code)]` stays.
- No new allocation-limit knob and no reimplementation of
  `max_allocation_bytes`. Untrusted input stays bounded on the new path by
  the existing block cap plus buffer containment.
- Existing `Value`-returning APIs keep working unchanged. This is an added
  path, not a migration.

## Design

### The architectural decision this reverses

doc/specs/AvroStreamingIO.md recorded, deliberately: *"the binding owns only
the Object Container File framing ... and delegates every decode step to
apache-avro's public API."* Both stages below require this crate to own a
schema-driven **byte-level datum reader**, because neither projection-skip
nor token emission can be expressed through `from_avro_datum_schemata`, which
only ever hands back a finished `Value`.

That means owning: zigzag varint decoding, blocked arrays and maps (including
the negative-count-plus-byte-size form that permits skipping), union branch
dispatch, `bytes`/`string` length prefixes, and `fixed` widths. It does
**not** mean owning schema parsing, logical-type interpretation, the
container framing, or the allocation bound (see below), all of which stay
where they are.

This is a real increase in owned surface and the main thing to accept or
reject about this spec.

### Allocation bounds: no new limit, no reimplemented policy

Constraint from review: `max_allocation_bytes` is not to be reimplemented.

Reusing upstream's check directly is not available - `apache_avro::util::safe_len`
is `pub(crate)` (util.rs:159), and only the `max_allocation_bytes` setter is
public. So the new path cannot call into upstream's enforcement.

It does not need to. Owning the reader supplies a **strictly tighter bound
for free**: every length prefix in a datum must fit within the bytes
remaining in its containing buffer. A container block holds a whole number of
complete datums, so a length prefix larger than the rest of the block is
malformed by definition, not merely suspicious. The same holds for the
single-datum path, where the containing buffer is the caller's `&[u8]`.

That is a structural invariant of the format, not a configurable policy. It
introduces no new tunable, no new global, and no second source of truth about
what "too big" means. The chain that bounds total allocation on the new path:

1. Block bytes are already capped by this crate's own
   `set_max_block_size` (container.rs:315, default 128 MiB), enforced before
   any buffering or allocation.
2. Every length prefix within a block is bounded by the bytes remaining in
   that block.
3. Therefore peak allocation for one datum is O(block bytes), with no
   attacker-controlled length prefix ever honored.

Projection makes this better rather than worse: a skipped subtree allocates
nothing at all, so the reachable allocation on a projected read is bounded by
the projection, not by the writer schema.

**Behavioural difference to document, not paper over.** The new path is
governed by `set_max_block_size` plus the containment invariant, *not* by
`set_max_allocation_bytes`. The existing apache-avro-backed paths are
untouched and keep their current semantics, and rust/tests/max_allocation.rs
keeps testing them. For container reads the new bound is the tighter of the
two (128 MiB block cap against a 512 MiB default allocation cap), but it is a
different knob and the README's "Untrusted input" section must say so.
An equivalent of rust/tests/max_allocation.rs is owed for the new path,
asserting that an over-length prefix is rejected before allocating.

### Shared core: a skipping cursor

One new module, `rust/decode.rs`, holding a reader driven by a pre-compiled
plan derived from the writer schema. Compiling the plan once per schema (not
per datum) is what removes the per-leaf field-name string comparison that
makes `AvroPath` traversal expensive today.

For each schema node the plan records whether the subtree is **wanted** or
**skippable**, and for skippable subtrees whether it can be skipped by a
constant byte width (`int`/`long` no, `fixed`/`float`/`double` yes) or must be
walked. Blocked arrays and maps written with a byte-size prefix can be
skipped with a single seek, which is where most of the projection win comes
from on manifests.

### Stage 1: projection pushdown

The smallest API that captures the largest measured win. The caller names the
fields it wants; everything else is skipped at the byte level and never
becomes a `Value`.

The C++ surface is a reader constructor taking a projection, and the result
is still an ordinary `AvroValue` holding only the projected fields. No new
value-access concepts, no token-cursor lifetime rules, and every existing
accessor keeps working on the result.

Deliberately *not* reusing the existing `FromBytesWithSchema` reader-schema
path: that means Avro schema resolution, with defaults for missing fields and
type promotion, and conflating "resolve to a different schema" with "decode
less of this schema" would make both harder to reason about. A projection is
a subset of the writer schema, checked against it at construction.

Expected result, from the avrocpp ceiling: 4.2x-4.8x on the projected read,
and the read-back cost largely disappears with it because there is far less
tree to walk.

### Stage 2: token cursor

Once the skipping cursor exists, exposing it as a pull-based token cursor is
mostly API surface: `Next()` advancing over
`StartRecord`/`Field`/`Long`/`Bytes`/`EndRecord`/... with the scalar payload
read off the cursor, plus `SkipValue()` to drop a subtree.

Pull-based, not callback-based (SAX proper), for the reason
doc/specs/AvroStreamingIO.md already rejected callbacks: Crubit
function-pointer bridging is unproven here, and Rust-calls-C++ re-entrancy
breaks the `catch_panic` model. A pull cursor keeps the control flow in C++
with no re-entrancy.

**Zero-copy is a stage 2 question, and an open one.** Today's accessors
return `VecU8` by value, so every string and byte payload is copied across
the FFI boundary. Handing C++ a `&[u8]` borrowed from the decompressed block
buffer is the actual zero-copy win, and it needs the borrow to stay valid
only until the cursor advances. Whether Crubit can express that lifetime
safely is unproven in this repo and must be prototyped before stage 2 is
committed to. If it cannot, stage 2 still pays for itself on allocation count
alone, but "zero-copy" should be dropped from its description rather than
quietly redefined.

### Why staged

Stage 1 delivers the biggest measured multiplier behind the smallest API, and
proves the shared core against a real manifest. Stage 2 is then additive and
can be judged on its own evidence. Landing the token cursor first would mean
designing the larger surface before the core it sits on has been validated,
and the measurement says the surface is not where the win is.

## Out of scope

- Changing the default `Value`-returning path or deprecating any existing
  API.
- Owning schema parsing, logical-type interpretation, or container framing.
- Writer-side work. Every number here is a read-path number.
- Multi-threaded or vectorized decode.
- Upstreaming any of this to apache-avro, though the projection gap is a
  legitimate upstream issue and worth filing separately.

## Open questions for review

1. Is owning a byte-level datum decoder acceptable, given
   doc/specs/AvroStreamingIO.md explicitly chose not to? If not, the ceiling
   is roughly the 1.3-2.3x of the AST build and neither stage is worth
   building.
2. ~~Stage 1 only, or commit to both now?~~ Stage 1 shipped. Stage 2 is now
   *less* attractive than when this was written: projection alone put us
   ahead of avrocpp, so a token cursor would be optimizing a path that is no
   longer the bottleneck. Worth re-measuring a real workload before
   committing to the larger API surface.
3. ~~How are projections expressed at the C++ boundary?~~ Resolved: a
   projected schema, checked as a subset of the writer schema at compile
   time. It reuses `AvroSchema::Parse`, needs no new syntax, and makes the
   subset rule explicit. Records may be narrowed; unions may not (branch
   indices are encoded in the data).
4. ~~Should `max_allocation_bytes` become per-reader?~~ Resolved: it is not
   reimplemented and not extended. The new path relies on the existing
   `set_max_block_size` cap plus the buffer-containment invariant; the
   existing paths keep upstream's global unchanged. See "Allocation bounds"
   above. Remaining sub-question: is a 128 MiB default block cap the right
   effective ceiling for the new path, given it now carries more of the
   safety argument than it used to?
