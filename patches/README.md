# Apache Avro patches

Patches this repo maintains against `apache-avro` (avro-rs), each a normal
`git format-patch` artifact applicable with `git am` or `git apply`.

| Patch | Base | Purpose |
|---|---|---|
| `apache-avro-0.21-non-utf8-string.patch` | `rel/release-0.21.0` | Adds `util::set_non_utf8_string_as_bytes`, off by default. On, a `string` whose wire bytes are not valid UTF-8 decodes as `Value::Bytes` instead of failing. Closes D1; see `doc/specs/AvroStringPolicy.md` |
| `apache-avro-0.21-uuid-as-string.patch` | the non-UTF-8 patch above | Adds `util::set_uuid_as_string`, off by default. On, a `uuid` decodes as an ordinary string, so the bytes as written survive: no canonical rewriting, no reinterpretation of a 16-byte string, no rejection of text that is not a uuid |
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

## apache-avro-0.21-uuid-as-string.patch

Base: the non-UTF-8 patch above, not the release tag. Both touch `decode.rs`,
`encode.rs`, `types.rs` and `util.rs`, so stacking them avoids a conflict; apply
them in order.

```sh
git clone https://github.com/apache/avro-rs && cd avro-rs
git checkout rel/release-0.21.0
git am /path/to/apache-avro-0.21-non-utf8-string.patch
git am /path/to/apache-avro-0.21-uuid-as-string.patch
cargo test -p apache-avro --features derive
```

Verified on 2026-08-17: `cargo fmt` clean, and the suite goes from 551 with the
non-UTF-8 patch alone to 558, the 7 tests this one adds. The same two
pre-existing failures noted above still need `--features derive`.

**The behaviour is off by default**, same shape as the patch above:

```rust
assert!(apache_avro::util::set_uuid_as_string(true));
```

Why it exists. Avro defines `uuid` as an annotation on `string`, and a reader
may leave the annotation uninterpreted; avro-cpp does, and never validates one.
Parsing it into a `Uuid` has three effects that reading it as a string does not,
each measured against avro-cpp with the differential fuzzer:

- **The bytes change.** `Uuid` re-encodes in canonical hyphenated lowercase, so
  `urn:uuid:` prefixes, braces, upper-case hex and unhyphenated 32-hex forms are
  all rewritten. A value read and written back is not the value that arrived.
- **A 16-byte string is reinterpreted.** Any `uuid` field of exactly 16 bytes is
  taken as a raw `Uuid` and comes back as 36 characters of hex. Length decides
  how the field is read, which also means the UTF-8 check is skipped at exactly
  that length.
- **Malformed text is rejected**, making data other implementations wrote
  unreadable.

What it changes, all in `avro/src/`:

- `util.rs`: the `UUID_AS_STRING` `OnceLock`, its `set_uuid_as_string` setter and
  crate-internal reader, shaped after the setting above.
- `decode.rs`, a guarded `Schema::Uuid` arm placed before the existing one: with
  the setting on it delegates to the `Schema::String` path.
- `types.rs`, `resolve_uuid`: with the setting on a `String` stays a `String`,
  which is what stops resolution reintroducing the canonicalisation.
- `types.rs`, `validate`, and `encode.rs`, the `Value::Bytes` arm: accept
  `Schema::Uuid`, for the non-UTF-8 case.

It composes with `set_non_utf8_string_as_bytes` rather than duplicating it. A
`uuid` whose text is not valid UTF-8 yields `Value::Bytes` when that setting is
also on, and still fails when it is not; `non_utf8_still_fails_without_the_other_setting`
pins that the two stay independent. `Value::String` was already accepted at a
`Schema::Uuid` position by `validate` and the encoder, so nothing there needed
widening.

Tests follow the same split: the parsing behaviour stays covered by the existing
unit tests at the default, and the accepting cases live in
`avro/tests/uuid_as_string.rs`, because a `OnceLock` cannot be reset in-process.

No public signature changes and no new error variants.

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
