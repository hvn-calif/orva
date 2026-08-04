# Benchmark: this binding vs Apache avro-cpp

Throughput comparison between this repo's Rust-backed Avro binding and
Apache avro-cpp `release-1.11.4`, per doc/specs/AvroBenchmark.md. Numbers
are informational; there is no pass/fail threshold.

## Prerequisites

- `boost-devel` (avro-cpp requires Boost >= 1.38: filesystem, iostreams,
  program_options, regex, system)
- `snappy-devel` (without it avro-cpp silently drops its snappy codec and
  the benchmark's startup validation fails on the snappy rows)
- `xz-devel`, `libzstd-devel`, `bzip2-devel` (EL9's Boost::iostreams CMake
  target declares them in its link interface)

## Build and run

```sh
cmake -B build -DAVRO_BUILD_BENCHMARKS=ON
cmake --build build --target avro_benchmark -j"$(nproc)"
./build/avro_benchmark
```

Startup first runs a cross-read validation circle per dataset and codec:
our writer's file (2000 values, spanning multiple container blocks) is
read by avrocpp, re-written by avrocpp, read back by us both whole-buffer
and through the chunked stream path (odd 4099-byte chunks, hitting
arbitrary split points), and every value compared. Any mismatch aborts, so
the timings can only ever compare two implementations doing identical
work.

## What is measured

Operations x datasets x codecs, single-threaded, all IO in memory:

- `container_write`: append all values and finish a container file.
- `container_read`: iterate every value of a container file. Both
  libraries read the same bytes (the avrocpp-written file).
- `stream_read_64k` (ours only): the same read through
  `DataFileStreamReader` fed 64 KiB chunks; compare against
  `ours/container_read` to see the push-parser overhead and against
  `avrocpp/container_read` for the end-to-end picture.
- `datum_encode` / `datum_decode`: raw single-datum binary encoding, no
  container framing, no codec. The decode side has three `ours*` rows:
  `ours` is the `DecodeDatum` free function, `ours_reader` is
  `AvroDatumReader::Decode` (writer schema resolved once), and
  `ours_reader_into` is `AvroDatumReader::DecodeInto` (resolved once and
  decoding into one caller-owned value). Only the last is like-for-like with
  the avrocpp row, which builds one `GenericReader` and reuses one
  `GenericDatum`. Findings in `benchmarks/datum_decode_results.txt`: the free
  function is 1.15-1.97x slower than avrocpp, holding a reader brings that to
  parity or 1.07x, and adding value reuse is 1.74-2.06x faster than avrocpp.
  Caching schema resolution alone removes about 65 ns per datum, near-identical
  on two datasets whose per-record work differs by 2x, which is why
  `DecodeDatum` looked worst on the cheapest records. See
  doc/specs/AvroDatumReader.md.

Datasets: `flat` (100k small records), `nested` (10k records with an
8-item array of sub-records and a 4-key map), `strings` (10k records with
a 1 KiB string).

Codecs: `null`, `deflate`, `snappy` (container operations only).

Interpretation caveats:

- Our `Append` validates every value against the writer schema; avrocpp's
  `write` does not validate `GenericDatum`. That is a real API difference
  and is deliberately included in the write numbers.
- Our datum API is one buffer per datum (`EncodeDatum`/`DecodeDatum`
  return/take whole buffers); avro-cpp encodes a stream of datums into one
  buffer. Each side runs in its natural mode, so the datum rows compare
  API shapes as much as codecs.
- Every value crosses the C++/Rust FFI boundary individually on our side.
- Write rows report MiB/s of bytes *produced* (post-compression), so
  within one library the deflate/snappy write MiB/s is not comparable to
  the null row as "input compressed per second". The ours-vs-avrocpp ratio
  per row is unaffected (both sides count the same way).
- avro-cpp's `close()` unavoidably re-flushes an empty block (codec call,
  sync marker) after the benchmark's own `flush()`; that small one-sided
  cost is included in the avrocpp write rows.

## Probes

Narrower questions the matrix above cannot answer. Same build flag, one
target each.

- `block_probe`: separates container block-framing cost from per-value decode
  cost, plus a block-count sweep and a contiguous struct-array traversal for
  scale. Marginal cost per block came out at roughly 21 ns for this binding
  against 37 ns for avrocpp, so block count is not a source of the read gap.
- `avro_sweep`: how the read gap scales with payload size (0-4096 bytes) and
  field count (1-16). Distinguishes a fixed per-record overhead from per-field
  and per-byte costs, which the single-shape matrix rows cannot.
- `append_probe`: what the value copy inside `Append` costs and what
  `Append(AvroValue&&)` saves, by building each record inside the timed
  region. The matrix rows above append the same values every iteration, so
  they cannot show this.
- `manifest_probe`: where the time goes on a real Apache Iceberg v2
  `manifest_entry` shape, which is the workload that prompted the
  investigation. Iceberg encodes its per-column metrics as
  `array<record{key,value}>` (Avro map keys must be strings), so a table with
  N columns puts 6N two-field sub-records in every entry. Set
  `MANIFEST_ENTRIES` (default 2000) and `MANIFEST_COLUMNS` (default 20);
  `MANIFEST_ENTRIES=128247` is the ~150 MB case. Findings are in
  doc/specs/AvroTokenStream.md; the short version is that building and
  dropping a fresh value tree is 1.3-2.3x slower than avrocpp, while reusing
  that tree makes Rust faster on decode and planner work. Byte-level
  projection and efficient read-back remain separate performance concerns.

  The `ours/project_native` row measures the projected reader added for that
  spec (`DataFileReader::CreateWithProjection`, rust/decode.rs). It reads the
  same planner fields 12.2x faster than materializing the whole entry at 20
  columns, and 1.87x faster than avrocpp's own projected read; the margin
  widens with table width because its allocation count is flat in it. The
  `ours/project_decode` row is kept for contrast: it is the same projection
  expressed as a reader schema, which apache-avro resolves *after*
  materializing everything, and is therefore slower than not projecting.

  The `ours_reuse/*` rows use `DataFileReader::NextValueInto`, overwriting one
  caller-owned `AvroValue` exactly as avro-cpp overwrites one
  `GenericDatum`. The reusable binary decoder lives in apache-avro;
  orva only caches its owning datum reader at the container boundary.
  Same-process 20-column medians (2000 entries, null codec, seven
  repetitions):

  | work | owned value | reusable value | avrocpp | reusable vs avrocpp |
  |------|------------:|---------------:|--------:|---------------------:|
  | decode only | 76.75k/s | **377.2k/s** | 109.09k/s | **3.46x faster** |
  | planner walk | 68.78k/s | **257.0k/s** | 108.66k/s | **2.37x faster** |
  | full metrics walk | 41.18k/s | **73.69k/s** | 104.53k/s | **1.42x slower** |

  Reuse therefore reverses the full-materialization *decode* comparison, but
  not an exhaustive read-back comparison: thousands of `AvroPath` leaf calls
  still cross and re-walk the Rust value tree. Projection remains faster when
  only planner fields are required because it does not decode the metrics
  bytes at all. The allocation test measures **0 allocator operations and
  0 allocated bytes** for the second same-shaped 20-column entry after
  warm-up, against 464 operations and 32,350 bytes for an owned decode. Raw
  throughput output and the exact command are in
  `benchmarks/manifest_reuse_results.txt`.
- `access_probe`: what reading a decoded value back out costs. Every other
  benchmark here decodes a value and drops it without reading a field, which
  hid the largest single cost in the binding: with the cloning accessors,
  reading every leaf of a record holding an 8-item array and a 4-key map cost
  1975 ns, more than the 1212 ns it took to decode, against 28 ns for
  avrocpp. `AvroPath` brings that to 1085 ns. It also measures a minimal FFI
  crossing at 1.34 ns, which is why batching the value-pulling API is not
  worth doing: the cost is copying, not crossing.

## Exporting results as TSV

```sh
./build/avro_benchmark --benchmark_format=json | jq -r '
  .benchmarks[] |
  [.name, (.real_time | tostring), .time_unit,
   ((.items_per_second // 0) | tostring),
   ((.bytes_per_second // 0) / 1048576 | tostring)] | @tsv' \
  > benchmark_results.tsv
```

Columns: name, wall time, unit, values/sec, MiB/sec.
