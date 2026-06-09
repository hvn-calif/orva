/// Byte-vector type used for both binary data and strings crossing the
/// C++ boundary.
/// NOTE: b/400396686 - Change once String is supported by Crubit.
use crate::make_vec_type;

make_vec_type!(u8, VecU8);

impl From<&str> for VecU8 {
    fn from(item: &str) -> Self {
        item.as_bytes().into()
    }
}

impl From<String> for VecU8 {
    fn from(item: String) -> Self {
        item.into_bytes().into()
    }
}

/// Result type for fallible operations that return no value.
/// NOTE: b/517030085 - Crubit doesn't support () here, so using a u8.
pub type Status = Result<u8, VecU8>;

/// Validates that `data` is UTF-8 and returns it as a `&str`, with the
/// error converted to the boundary error type.
pub(crate) fn utf8(data: &[u8]) -> Result<&str, VecU8> {
    std::str::from_utf8(data).map_err(|err| err.to_string().into())
}

/// Runs `f`, turning any Rust panic into a boundary error instead of
/// letting it unwind across the FFI boundary. A panic crossing the
/// Crubit `extern "C"` thunk aborts the whole process (and is undefined
/// behavior under `panic = "unwind"`), so a single malformed input would
/// take down the host. apache-avro panics on some malformed schemas and
/// data (and several codec paths `unwrap` decompressor errors), so every
/// entry point that touches untrusted bytes must run through this guard.
/// Mirrors `pixel_bridge`'s `run_catching_panics`.
pub(crate) fn catch_panic<T>(f: impl FnOnce() -> Result<T, VecU8>) -> Result<T, VecU8> {
    // AssertUnwindSafe is sound here: every guarded operation either
    // produces a fresh value or fails, so a caught panic leaves no
    // half-updated shared state observable to the caller.
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(payload) => Err(panic_detail(payload).into()),
    }
}

/// Extracts a human-readable message from a caught panic payload.
fn panic_detail(payload: Box<dyn std::any::Any + Send>) -> String {
    let detail = payload
        .downcast_ref::<&str>()
        .map(|s| (*s).to_string())
        .or_else(|| payload.downcast_ref::<String>().cloned())
        .unwrap_or_else(|| "unknown panic payload".to_string());
    format!("Rust panic caught while processing Avro input: {detail}")
}
