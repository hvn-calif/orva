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
  container framing, no codec.

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
