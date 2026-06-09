# PNG Text Metadata in pixel_bridge

## Purpose & user problem

`pixel_bridge` already surfaces structured image metadata (ICC profile, Exif,
XMP, IPTC) to C++ callers. It does not surface PNG textual metadata, which lives
in PNG's `tEXt`, `zTXt`, and `iTXt` chunks. These chunks carry human- and
tool-authored key/value information (e.g. `Software`, `Comment`, `Description`,
`Author`, AI-generation provenance fields, etc.). Callers that need to inspect
or audit that text currently cannot, because the binding never exposes it.

This feature adds a way for C++ callers to read PNG text chunks as a list of
keyword/value pairs.

## Background: why this is not a one-line delegation

The existing metadata accessors delegate to the `image` crate's uniform
`ImageDecoder` trait (`icc_profile()`, `exif_metadata()`, ...), each returning a
single `Option<Vec<u8>>` that crosses FFI as one `VecU8` blob.

PNG text chunks are **not** part of that trait. They live in the underlying
`png` crate's `Info` struct (`uncompressed_latin1_text`,
`compressed_latin1_text`, `utf8_text`), which `image::codecs::png::PngDecoder`
does not expose. So this feature must reach the `png` crate directly.

There is an existing precedent for this in `rust/reader.rs`: the TIFF builder
(`fn tiff`, lines ~162-188) constructs the underlying `tiff` crate decoder from
the raw reader to read an `ExtraSamples` tag the `image` crate hides, then seeks
the reader back to start before handing it to the `image` crate. The PNG text
path follows the same shape.

## Success criteria

- A C++ caller holding an `ImageDecoder` for a PNG can call a new method and get
  back the image's text chunks as a `std::vector` of keyword/value pairs.
- All three textual chunk types are covered: `tEXt`, `zTXt` (decompressed), and
  `iTXt` (UTF-8).
- `tEXt`/`zTXt` keyword and text (Latin-1 / ISO-8859-1) are transcoded to UTF-8
  so the returned `std::string`s are valid UTF-8. `iTXt` is already UTF-8.
- For non-PNG images, the method returns an empty vector (never an error).
- For a PNG with no text chunks, the method returns an empty vector.
- Malformed or undecodable individual text chunks are skipped rather than
  failing the whole decode or the whole call (best-effort), consistent with the
  crate's existing panic-catching posture (`run_catching_panics`).
- The repository still builds under the existing CMake/Crubit CI matrix.

## Scope

### API shape (decided: keyword + value pairs)

C++ side, in `namespace security::pixel_bridge` (header `pixel_bridge.h`):

```cpp
// A single PNG text chunk surfaced as a keyword/value pair. tEXt, zTXt, and
// iTXt chunks are all folded into this flat representation; keywords may repeat
// (e.g. multiple "Comment" entries). Both fields are UTF-8.
struct PngTextChunk final {
  std::string keyword;
  std::string text;
};

// ... on class ImageDecoder:

// Returns the PNG text chunks (tEXt/zTXt/iTXt) embedded in the image as a flat
// list of keyword/value pairs, in file order. Returns an empty vector for
// images that are not PNG or that carry no text chunks. Chunks that cannot be
// decoded are skipped.
//
// Note: Only relevant for PNG images (mirrors HasPalette()).
std::vector<PngTextChunk> GetPngTextMetadata();
```

Folding the three chunk kinds into one flat `{keyword, text}` list (rather than
a richer struct carrying chunk kind / iTXt language tag / translated keyword) is
the agreed shape. The chunk kind and iTXt language metadata are intentionally
dropped; see "Out of scope".

### Rust side

1. **Cargo.toml**: add `png` as a direct dependency, version-matched to the one
   `image` 0.25 already resolves (expected `png = "0.17"`), so the build does
   not pull a second incompatible `png` copy. Confirm the resolved version
   during implementation.

2. **New bridged type** (new module `rust/png_text.rs`, registered in
   `rust/lib.rs`), following the `VecU8` pattern (a `#[repr(C)]` newtype over a
   Rust collection, read by C++ through index/len accessor methods):

   ```rust
   #[repr(C)]
   #[derive(Default, Debug, Clone)]
   pub struct PngTextChunks(Vec<(String, String)>);

   impl PngTextChunks {
       pub fn len(&self) -> usize;
       pub fn is_empty(&self) -> bool;
       // Returns the keyword / text of entry `index` as UTF-8 bytes.
       // (Reuses VecU8 so C++ reads it with the existing helper.)
       pub fn keyword(&self, index: usize) -> VecU8;
       pub fn text(&self, index: usize) -> VecU8;
   }
   ```

   Returning `VecU8` per field (a small clone) keeps the FFI consistent with the
   existing string-passing helper and sidesteps lifetime-over-FFI issues, which
   the codebase already calls out as hard (`reader.rs` `new_in_memory`). Text
   metadata is small, so per-access cloning is acceptable.

3. **`rust/image.rs`**:
   - Add a field to `ImageDecoder`, e.g. `png_text: Vec<(String, String)>`,
     defaulted to empty in `new()`.
   - Add an accessor:
     ```rust
     pub fn png_text_chunks(&self) -> PngTextChunks {
         PngTextChunks(self.png_text.clone())
     }
     ```
     Non-fallible: parsing happens eagerly at construction (below), so by call
     time there is no fallible/moved-out state to surface. Empty == "none".

4. **`rust/reader.rs`** `fn png(...)`: mirror the TIFF precedent.
   - Take the reader by value, parse text chunks from it best-effort
     (wrapped so a bad chunk cannot break image decoding), `seek(Start(0))`,
     then construct the `image` `PngDecoder` as today, and store the parsed
     entries on the returned `ImageDecoder`.
   - Parsing helper uses the `png` crate:
     `png::Decoder::new(&mut *reader).read_info()`, then reads
     `info().uncompressed_latin1_text` (`tEXt`),
     `info().compressed_latin1_text` (`zTXt`, decompressed via the chunk's
     `get_text()`), and `info().utf8_text` (`iTXt`, via `get_text()`).
     `tEXt`/`zTXt` keyword and text are transcoded Latin-1 -> UTF-8 by mapping
     each byte to the matching Unicode code point
     (`bytes.iter().map(|&b| b as char).collect()`).
   - The exact `png` 0.17 type/method names (`text_metadata::TEXtChunk`,
     `ZTXtChunk::get_text`, `ITXtChunk::get_text`, and the `Info` field names)
     must be verified against the resolved crate version during implementation.

### C++ side

- `pixel_bridge.h`: declare `struct PngTextChunk` and the
  `GetPngTextMetadata()` method.
- `pixel_bridge.cc`: implement by calling `decoder_.png_text_chunks()` and
  iterating `0..len()`, building each `PngTextChunk` from `keyword(i)` /
  `text(i)` using the existing `VecU8` -> string conversion (the file already
  has `StringViewFromVecU8`; reuse it to construct `std::string`s).

### Build

No CMake change is expected: the new bridged type and method are generated by
Crubit from the Rust crate, and `std::vector` / `std::string` are already used
in the header. (The recently fixed Abseil linking in `pixel_bridge/CMakeLists.txt`
is unrelated and already in place.)

## Out of scope

- Chunk **kind** (whether an entry came from `tEXt`, `zTXt`, or `iTXt`) is not
  surfaced.
- `iTXt` **language tag** and **translated keyword** are not surfaced (only the
  keyword and the UTF-8 text).
- **Writing/encoding** PNG text chunks. Read-only.
- Text chunks placed **after `IDAT`**: parsing uses `read_info()`, which
  captures chunks appearing before the image data (the standard placement and
  what common encoders emit). Capturing trailing text chunks would require
  reading the image stream to completion a second time and is deferred. This
  limitation will be noted in the method's doc comment.
- Text metadata for non-PNG formats (e.g. JPEG comments, GIF comments). PNG
  only, as requested.
- New unit/integration tests and sample fixtures, unless requested. CI today
  only builds; there is no test harness in this binding.

## Open questions / risks

1. **`png`-crate API drift**: exact `Info` field names and the
   `get_text()`/decompression entry points differ across `png` versions. Must be
   confirmed against the version `image` 0.25 resolves. (Cannot inspect the
   vendored source in the current environment; CI/the dev machine will resolve
   it.)
2. **Crubit bridging of the index/len accessor pattern**: `VecU8` proves a
   `#[repr(C)]` newtype with `as_ptr`/`len` bridges cleanly. `PngTextChunks`
   adds index methods that take a `usize` and return `VecU8` by value; this
   should bridge but needs to be confirmed by a build during implementation.
3. **Method naming**: `GetPngTextMetadata` chosen for symmetry with
   `GetExifMetadata` etc. while signaling PNG specificity. Open to
   `GetTextChunks` if preferred.
