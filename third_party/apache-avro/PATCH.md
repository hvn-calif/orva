# Local fork of apache-avro 0.21.0

Vendored from crates.io at version 0.21.0, with two additions and nothing
else. `diff -r` against the pristine crate touches exactly three files.

## Why

Every public per-datum decode entry point in upstream rebuilds a
`ResolvedSchema` on each call:

- `decode::decode` (`src/decode.rs:74`) -> `ResolvedSchema::try_from(schema)`
- `from_avro_datum` -> `decode`
- `from_avro_datum_schemata` -> `from_avro_datum_reader_schemata`
  (`src/reader.rs:492`) -> `ResolvedSchema::try_from(writer_schemata)`

That rebuild allocates a `HashMap` and walks the whole schema tree before a
single byte is read. For small records it costs more than the decode: on a
`{string username, int age}` record it is 100-113 ns of a ~179 ns decode.

Upstream's own container reader does not pay this -- `Block` resolves the
names once when it reads the header (`src/reader.rs:228`) and passes them to
`decode_internal` per value. Our push parser cannot reuse `Block` (it is
pull-based over `io::Read`, and has no cap on block size), and the machinery
`Block` uses is all `pub(crate)`: `decode_internal`, `Names`,
`resolve_names`, `ResolvedOwnedSchema::get_names`.

These additions expose the same capability upstream already uses internally.

## The patch

| file | change |
| --- | --- |
| `src/lib.rs` | `mod decode` -> `pub mod decode` (visibility only) |
| `src/schema.rs` | `Names` `pub(crate)` -> `pub` (visibility only); adds `build_names`, a wrapper over the unchanged `resolve_names` |
| `src/decode.rs` | adds `decode_resolved`, a wrapper over the unchanged `decode_internal` |

No existing function body is modified. `decode_resolved` reaches the same
`decode_internal` as `decode` does, so decoded values are identical.

## Equivalence

Checked against a self-referencing schema (`Node{value:int, next:[null,Node]}`,
3 levels deep), which is the case the `schemata` argument of
`from_avro_datum_schemata` exists to serve: `from_avro_datum_schemata` and
`decode_resolved` + `build_names` produce equal values and consume the same
byte count.

One difference worth knowing: `ResolvedSchema::try_from` builds its map with
`ResolvedSchema::resolve` (borrowed schemas, supports `known_schemata` for
cross-document references), while `build_names` uses the older
`resolve_names` (owned clones, no `known_schemata`). They perform the same
traversal for the single self-contained schema a container file header
carries, which is the only case `container.rs` uses. Anything needing
cross-document reference resolution must keep using the upstream entry point.

## Upstreaming

The additions are small and semantically inert; they belong upstream. Once
they are released, drop `third_party/apache-avro`, remove the
`[patch.crates-io]` section from `rust/Cargo.toml`, and bump the version
requirement.
