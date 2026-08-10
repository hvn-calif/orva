# Apache Avro patches

Patches this repo maintains against `apache-avro` (avro-rs), each a normal
`git format-patch` artifact applicable with `git am` or `git apply`.

| Patch | Base | Purpose |
|---|---|---|
| `apache-avro-0.21-non-utf8-string.patch` | `rel/release-0.21.0` | Decode a `string` whose wire bytes are not valid UTF-8 as `Value::Bytes` instead of failing. Closes D1; see `doc/specs/AvroStringPolicy.md` |
| `apache-avro-0.22-read-into.patch` | avro-rs `8000091350d32f4ed4d94166dcb7695a4a25e409` | Allocation-reusing value decoding: `read_into`, `read_value_into`, `OwnedGenericDatumReader` |

## apache-avro-0.21-non-utf8-string.patch

Base: the `rel/release-0.21.0` tag, which matches the crates.io `apache-avro
0.21.0` source byte for byte (verified against
`~/.cargo/registry/src/*/apache-avro-0.21.0/`). It applies to a clean checkout
of that tag with no other patch present, and does **not** depend on the
read-into patch below.

```sh
git clone https://github.com/apache/avro-rs && cd avro-rs
git checkout rel/release-0.21.0
git am /path/to/apache-avro-0.21-non-utf8-string.patch
cargo test -p apache-avro --features derive
```

Verified: applies clean via `git am`, and the suite passes at 428 tests, the
421 the release ships plus the 7 this patch adds. Two pre-existing failures in
that checkout are unrelated to the patch and reproduce without it: the
`specific_single_object` example and the `avro-rs-226` integration test both
need the `derive` feature, so build with `--features derive`.

What it changes, all in `avro/src/`:

- `decode.rs`, the `Schema::String` arm: invalid UTF-8 yields `Value::Bytes`.
- `decode.rs`, the map-key site: still rejects, but reports UTF-8 rather than
  reporting the key as the wrong type. Map keys cannot hold raw bytes because
  `Value::Map` is keyed by `String`.
- `encode.rs`, the `Value::Bytes` arm: `Schema::String` accepted, so such a
  value round-trips byte for byte.
- `types.rs`, `validate`: `Value::Bytes` satisfies `Schema::String`.
- `types.rs`, `resolve_string`: invalid bytes stay `Value::Bytes`; valid bytes
  still become a `String`.

No public signature changes and no new error variants, so it is additive for
any other consumer of the crate.

## apache-avro-0.22-read-into.patch

Targets 0.22 and adds the reusable decoder API (`Reader::read_into`,
`GenericDatumReader::read_value_into`, `OwnedGenericDatumReader`).

**This patch is not currently in use.** The 0.21 line is the target, and it is
raw 0.21 without this patch. Note what that costs, because it is not free:

`rust/datum.rs` and `rust/container.rs` call `OwnedGenericDatumReader` and
`read_value_into`, which raw 0.21 does not have. Building against raw 0.21
therefore requires reimplementing or removing:

- `AvroDatumReader` in `rust/datum.rs`, and with it `AvroDatumReader::Create`,
  `Decode`, `DecodeInto` and `WriterSchema` in `avro_bridge.h` (see
  `doc/specs/AvroDatumReader.md`).
- the allocation-reusing container read path in `rust/container.rs`
  (`reuse_decoder`), and with it `DataFileReader::NextValueInto`.

`AvroProjection` and the projected container read do not depend on this patch:
`rust/decode.rs` is a hand-written decoder that does its own byte walking.

Instructions for a receiving workspace, if this patch is ever reinstated:
check out avro-rs `8000091350d32f4ed4d94166dcb7695a4a25e409` (it also applies
to `006ac8976f52af356beb5042788370f645f6da02`, which differ only in a
dependency bump and added documentation, not in the files touched), apply the
patch, and redirect the `apache-avro` dependency to the patched crate with a
path dependency or Cargo source override. Do not commit the receiving
machine's checkout path to this repository.
