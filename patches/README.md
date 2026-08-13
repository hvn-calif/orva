# Apache Avro patches

Patches this repo maintains against `apache-avro` (avro-rs), each a normal
`git format-patch` artifact applicable with `git am` or `git apply`.

| Patch | Base | Purpose |
|---|---|---|
| `apache-avro-0.21-non-utf8-string.patch` | `rel/release-0.21.0` | Adds `util::set_non_utf8_string_as_bytes`, off by default. On, a `string` whose wire bytes are not valid UTF-8 decodes as `Value::Bytes` instead of failing. Closes D1; see `doc/specs/AvroStringPolicy.md` |
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

Verified on 2026-08-13: applies clean to a detached worktree at `0470799`
(`git apply --check`), `cargo fmt --check` clean, no new clippy findings in the
changed files, and the suite goes from 541 passing on the unpatched tag to 551
with the patch, the 10 tests it adds. Two pre-existing failures in that checkout
are unrelated and reproduce without the patch: the `specific_single_object`
example and the `avro-rs-226` integration test both need the `derive` feature,
so build with `--features derive`.

**The behaviour is off by default.** Nothing changes for an existing consumer of
the crate until a caller opts in, once per process and before any decoding:

```rust
// The return value is the setting actually in effect, which differs from the
// argument if something already set it. Check it rather than assume.
assert!(apache_avro::util::set_non_utf8_string_as_bytes(true));
```

What it changes, all in `avro/src/`:

- `util.rs`: the `NON_UTF8_STRING_AS_BYTES` `OnceLock`, its
  `set_non_utf8_string_as_bytes` setter and crate-internal reader. Shaped after
  the existing `set_serde_human_readable`.
- `decode.rs`, the `Schema::String` arm: with the setting on, invalid UTF-8
  yields `Value::Bytes`; off, it fails as before.
- `decode.rs`, the map-key site: still rejects either way, but reports UTF-8
  rather than reporting the key as the wrong type. Map keys cannot hold raw
  bytes because `Value::Map` is keyed by `String`.
- `encode.rs`, the `Value::Bytes` arm: with the setting on, `Schema::String` is
  accepted, so such a value round-trips byte for byte.
- `types.rs`, `validate`: with the setting on, `Value::Bytes` satisfies
  `Schema::String`.
- `types.rs`, `resolve_string`: with the setting on, invalid bytes stay
  `Value::Bytes`. Valid bytes still become a `String` either way.

Tests are split by the value of the setting, because a `OnceLock` cannot be
reset within a process: the rejecting cases are unit tests in `decode.rs` and
`types.rs` (default), and the accepting cases are their own test binary,
`avro/tests/non_utf8_string_as_bytes.rs`, mirroring how upstream tests
`serde_human_readable` with one binary per value.

No public signature changes and no new error variants, and the default preserves
every existing behaviour, so it is additive for any other consumer of the crate.

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
