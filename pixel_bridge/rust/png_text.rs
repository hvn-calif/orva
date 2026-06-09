//! Bridged representation of PNG textual metadata (`tEXt`/`zTXt`/`iTXt`).
//!
//! WARNING: This crate should never be used from Rust, instead use image/png
//! directly. This type exists only so Crubit can expose the text chunks to C++.

use crate::vec_u8::VecU8;

/// A flat list of PNG text chunks as `(keyword, text)` pairs, in file order.
///
/// `tEXt`, `zTXt`, and `iTXt` chunks are all folded into this single list.
/// Keywords may repeat (e.g. multiple `Comment` entries). Both the keyword and
/// the text are UTF-8: `tEXt`/`zTXt` are Latin-1 in the file but the `png`
/// crate decodes them to UTF-8, and `iTXt` is UTF-8 by definition.
///
/// C++ reads the chunks by index, mirroring the `VecU8` access pattern: call
/// `len()`, then `keyword(i)` / `text(i)` for each entry.
#[repr(C)]
#[derive(Default, Debug, Clone)]
pub struct PngTextChunks(Vec<(String, String)>);

impl PngTextChunks {
    /// Number of text chunks.
    pub fn len(&self) -> usize {
        self.0.len()
    }

    /// Whether there are no text chunks.
    pub fn is_empty(&self) -> bool {
        self.0.is_empty()
    }

    /// Keyword of the entry at `index` as UTF-8 bytes. Returns an empty buffer
    /// if `index` is out of bounds.
    pub fn keyword(&self, index: usize) -> VecU8 {
        match self.0.get(index) {
            Some((keyword, _)) => keyword.as_str().into(),
            None => VecU8::default(),
        }
    }

    /// Text of the entry at `index` as UTF-8 bytes. Returns an empty buffer if
    /// `index` is out of bounds.
    pub fn text(&self, index: usize) -> VecU8 {
        match self.0.get(index) {
            Some((_, text)) => text.as_str().into(),
            None => VecU8::default(),
        }
    }
}

impl From<Vec<(String, String)>> for PngTextChunks {
    fn from(entries: Vec<(String, String)>) -> Self {
        Self(entries)
    }
}
