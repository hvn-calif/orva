//! This file provides macros for creating Crubit-compatible `Vec` types.
//! Copied from the serde_json binding (which adapted it from
//! //base/rust/rust_vec_u8.rs); see NOTE: b/399861833.

/// Macro for generating a Crubit-compatible `Vec` type. The `Vec` type
/// has similar methods as a Rust `Vec` and converts to/from the `Vec` type.
///
/// # Prerequisites
/// The element type needs to implement `Debug`, to be able to print the
/// vector, `Clone` to implement to_vec, and `PartialEq` to be an element
/// of the vector.
///
/// # Example
/// ```rust
/// struct Foo {
///    a: u32,
/// }
/// make_vec_type!(Foo); // Generates a `VecFoo` that can be bridged to C.
/// // Alternatively define your own name for the Vec type.
/// make_vec_type!(u32, VecU32);
/// ```
#[macro_export]
macro_rules! make_vec_type {
    ($type:ty, $vec_name:ident) => {
        #[repr(C)]
        #[derive(Default, Debug, Clone, PartialEq)]
        pub struct $vec_name(Vec<$type>);

        impl $vec_name {
            #[doc="See [std::vec::Vec::new](https://doc.rust-lang.org/std/vec/struct.Vec.html#method.new)."]
            pub fn new() -> $vec_name {
                $vec_name(Vec::new())
            }

            #[doc="Converts into the underlying `Vec<$type>`."]
            pub fn into_vec(self) -> Vec<$type> {
                self.0
            }

            #[doc="Views this as a slice of `$type`."]
            pub fn as_slice(&self) -> &[$type] {
                &self.0
            }

            #[doc="Creates a new vector with the contents of `value`.
                   This is the constructor to use from C++."]
            pub fn copy_from_slice(value: &[$type]) -> $vec_name {
                $vec_name(value.to_vec())
            }

            #[doc="See [std::vec::Vec::as_ptr](https://doc.rust-lang.org/std/vec/struct.Vec.html#method.as_ptr)."]
            pub fn as_ptr(&self) -> *const $type {
                self.0.as_ptr()
            }

            #[doc="See [std::vec::Vec::len](https://doc.rust-lang.org/std/vec/struct.Vec.html#method.len)."]
            pub fn len(&self) -> usize {
                self.0.len()
            }

            #[doc="See [std::vec::Vec::is_empty](https://doc.rust-lang.org/std/vec/struct.Vec.html#method.is_empty)."]
            pub fn is_empty(&self) -> bool {
                self.0.is_empty()
            }
        }

        impl From<Vec<$type>> for $vec_name {
            fn from(value: Vec<$type>) -> Self {
                $vec_name(value)
            }
        }

        impl From<&[$type]> for $vec_name {
            fn from(value: &[$type]) -> Self {
                $vec_name(value.to_vec())
            }
        }
    };
    ($vec_name:ident) => {
        paste::paste! {
          make_vec_type!($vec_name, [<Vec$vec_name>]);
        }
    };
}

#[cfg(test)]
mod test {
    make_vec_type!(u32, VecU32);

    #[test]
    fn test_vec_roundtrip() {
        let v = VecU32::from(vec![1, 2, 3]);
        assert_eq!(v.len(), 3);
        assert!(!v.is_empty());
        assert_eq!(v.as_slice(), &[1, 2, 3]);
        assert_eq!(v.clone().into_vec(), vec![1, 2, 3]);
        assert!(!v.as_ptr().is_null());

        let empty = VecU32::new();
        assert!(empty.is_empty());
        assert_eq!(empty.len(), 0);
    }
}
