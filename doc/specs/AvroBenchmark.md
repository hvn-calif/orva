# Spec: Benchmark: this binding vs Apache avro-cpp

Status: DRAFT (interview done 2026-07-24, awaiting GO)

## Purpose & user problem

This repo's memory-safe Avro binding (Rust apache-avro behind Crubit)
replaces avrocpp. We want numbers for what that replacement costs (or wins)
in throughput. Purpose is informational: a runnable comparison table for
humans; no pass/fail threshold (decided in interview).

## Success criteria

- One target, `avro_benchmark`, prints a comparison of the binding vs
  Apache avro-cpp doing identical work: same schemas, same values, same
  codec, same value counts, in-memory IO, single thread.
- Cross-read validation runs before timing: a 2000-value file (multiple
  container blocks, so block-boundary code paths are exercised) written by
  our writer is read by avrocpp, re-written by avrocpp, and read back by
  us both whole-buffer and through the chunked stream path (odd-size
  chunks); every value is compared and the benchmark aborts on any
  mismatch. This proves both sides do the same work before we time it.
- Normal builds are unaffected: the benchmark and its dependencies
  (avro-cpp, Boost, google-benchmark) are only configured with
  `-DAVRO_BUILD_BENCHMARKS=ON`.

## Benchmark matrix (decided in interview)

Operations:

1. Container write: append N values, finish, into an in-memory buffer.
2. Container read: iterate all values of an in-memory container file.
3. Streaming chunked read: our DataFileStreamReader fed 64 KiB chunks,
   measuring the push-parser overhead. avrocpp has no push API, so its
   comparison point is its own whole-buffer read (operation 2); the row
   exists to show what the chunked path costs relative to that.
4. Raw datum encode and decode (no container framing): EncodeDatum /
   DecodeDatum vs avrocpp's binary encoder/decoder over GenericDatum.

Datasets:

- flat: `{long id, string name}` with a 12-byte name; framing-bound.
  N = 100000.
- nested: record holding an `array<record{string, long}>` (8 items) and a
  `map<string, long>` (4 keys); allocation-bound. N = 10000.
- strings: record with one 1 KiB string field; memcpy-bound. N = 10000.

Codecs: null, deflate, snappy, for container operations (datum operations
have no codec). zstandard is excluded: avrocpp does not support it.

Metrics: wall time per operation plus derived values/sec and MB/sec
(google-benchmark `SetItemsProcessed`/`SetBytesProcessed`). Input
construction and validation are excluded from the timed region.

## Design

- `benchmarks/avro_benchmark.cc`: one file, flat structure. Per library a
  small set of free functions (write_container, read_container,
  encode_datums, decode_datums); benchmark registrations parameterized by
  dataset and codec. No adapter-class hierarchy: two libraries do not
  justify one.
- google-benchmark via FetchContent (v1.9.1, tests off).
- avro-cpp via FetchContent from the apache/avro repo, tag
  `release-1.11.4` (decided in interview: benchmark against the 1.11 line
  being migrated from; 1.11.4 is its latest patch), `SOURCE_SUBDIR
  lang/c++`. Verified requirements (release-1.11.4
  lang/c++/CMakeLists.txt): Boost >= 1.38 (filesystem, iostreams,
  program_options, regex, system), snappy optional via find_package,
  C++11 default standard (overridable; this tree builds C++20).
- avrocpp writes to `avro::memoryOutputStream()` and reads from
  `avro::memoryInputStream()`; our side uses the existing in-memory API.
  Neither side touches disk inside the timed region.
- Benchmark binary links both libraries; the two Avro implementations have
  disjoint symbol namespaces (C++ `avro::` vs our `security::avro::` +
  Crubit C symbols), so one binary is fine.

## Host prerequisites

- `boost-devel` and `snappy-devel` (EL9 AppStream; Boost 1.75 satisfies
  the 1.38 floor), plus `xz-devel`, `libzstd-devel` and `bzip2-devel`
  (EL9's Boost::iostreams link interface requires them). Install via sudo
  dnf was approved in the interview.
  Without snappy-devel, avro-cpp silently disables its snappy codec, so
  the snappy rows require it.
- If boost-devel cannot be installed, the benchmark target simply cannot
  build; building Boost from source is out of scope.

## Reporting

- google-benchmark console table, one row per
  `<library>/<operation>/<dataset>/<codec>`.
- `benchmarks/README.md` documents how to run and how to export results:
  `--benchmark_format=json` plus a documented jq one-liner producing a TSV
  with side-by-side ours-vs-avrocpp ratios.

## Out of scope

- Peak-memory (RSS) comparison. It is the streaming design's headline
  advantage, but measuring it well needs different instrumentation than a
  throughput harness; candidate follow-up spec.
- CI integration, regression thresholds, historical tracking.
- Multi-threaded or concurrent-reader scenarios.
- zstandard codec (avrocpp lacks it), bzip2/xz (both bindings lack them).

## Decisions from the interview (2026-07-24)

1. Operations: container write+read, raw datum encode/decode, and the
   streaming chunked read row. Codecs: null, deflate, snappy. Datasets:
   flat, nested, string-heavy. Informational only, no pass/fail gate.
2. avro-cpp pin: `release-1.11.4` (the 1.11 line being migrated from),
   not 1.12.
3. Host packages: installing boost-devel and snappy-devel via sudo dnf is
   approved.
