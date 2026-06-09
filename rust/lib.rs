//! This crate provides C++ bindings for the apache-avro crate.
//! This crate defines types and methods that expose the apache-avro API,
//! but in a way that Crubit understands and is able to generate valid C++
//! headers.
//!
//! WARNING: This crate should never be used from Rust, instead use
//! apache-avro directly.

#![forbid(unsafe_code)]

mod crubit_vec_util;

pub mod vec_u8;

pub mod schema;

pub mod value;

pub mod datum;

pub mod container;
