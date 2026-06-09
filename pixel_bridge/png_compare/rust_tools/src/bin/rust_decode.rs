// Decodes a PNG exactly the way pixel_bridge does: image crate's PngDecoder
// with read_image into a raw buffer (EXPAND transformation, no rgb->gray, no
// 16->8 strip). Emits a normalized line of logical samples so the output can be
// compared against the libpng harness regardless of byte endianness.
//
// Usage:
//   rust_decode <file.png>           native decode (what pixel_bridge surfaces)
//   rust_decode <file.png> --luma    native decode then downstream to_luma8
//                                     (the app-level grayscale path)

use image::codecs::png::PngDecoder;
use image::{ColorType, DynamicImage, ImageDecoder};
use std::env;
use std::fs::File;
use std::io::BufReader;

fn color_name(c: ColorType) -> (&'static str, u32, u32) {
    // (name, channels, bits-per-channel)
    match c {
        ColorType::L8 => ("Gray", 1, 8),
        ColorType::La8 => ("GrayA", 2, 8),
        ColorType::Rgb8 => ("Rgb", 3, 8),
        ColorType::Rgba8 => ("Rgba", 4, 8),
        ColorType::L16 => ("Gray", 1, 16),
        ColorType::La16 => ("GrayA", 2, 16),
        ColorType::Rgb16 => ("Rgb", 3, 16),
        ColorType::Rgba16 => ("Rgba", 4, 16),
        other => panic!("unexpected color type {:?}", other),
    }
}

fn main() {
    let args: Vec<String> = env::args().collect();
    let path = &args[1];
    let luma = args.get(2).map(|s| s == "--luma").unwrap_or(false);
    let name = std::path::Path::new(path)
        .file_name()
        .unwrap()
        .to_string_lossy()
        .to_string();

    let file = BufReader::new(File::open(path).unwrap());
    let decoder = PngDecoder::new(file).unwrap();
    let (w, h) = decoder.dimensions();
    let color = decoder.color_type();
    let (cname, channels, bits) = color_name(color);

    if luma {
        // Downstream grayscale path: decode to a DynamicImage then to_luma8,
        // which applies image's Rec.709 / 10000 integer luma (no gamma).
        let dynimg = DynamicImage::from_decoder(decoder).unwrap();
        let gray = dynimg.to_luma8();
        let samples: Vec<String> = gray.as_raw().iter().map(|v| v.to_string()).collect();
        println!(
            "# lib=image file={} mode=luma color=Gray depth=8 w={} h={} channels=1",
            name, w, h
        );
        println!("samples: {}", samples.join(" "));
        return;
    }

    // Native decode, exactly like pixel_bridge read_u8_slice / read_u16_slice.
    let total = decoder.total_bytes() as usize;
    let mut buf = vec![0u8; total];
    decoder.read_image(&mut buf).unwrap();

    let samples: Vec<String> = if bits == 16 {
        // pixel_bridge reinterprets the byte buffer as native-endian u16.
        buf.chunks_exact(2)
            .map(|b| u16::from_ne_bytes([b[0], b[1]]).to_string())
            .collect()
    } else {
        buf.iter().map(|v| v.to_string()).collect()
    };

    println!(
        "# lib=image file={} mode=native color={} depth={} w={} h={} channels={}",
        name, cname, bits, w, h, channels
    );
    println!("samples: {}", samples.join(" "));
}
