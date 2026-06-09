// Generates PNG fixtures with precise control over bit depth, color type,
// tRNS, and palette, using the same `png` crate version pixel_bridge resolves.
// Both decoders (image crate and libpng) consume these byte-identical files.

use png::{BitDepth, ColorType, Encoder, SrgbRenderingIntent};
use std::fs::File;
use std::io::BufWriter;
use std::path::Path;

fn writer(name: &str, w: u32, h: u32) -> Encoder<'static, BufWriter<File>> {
    let dir = Path::new("fixtures");
    let file = File::create(dir.join(name)).unwrap();
    Encoder::new(BufWriter::new(file), w, h)
}

fn main() {
    std::fs::create_dir_all("fixtures").unwrap();

    // 1) 8-bit RGB, distinct colors -- drives the color->grayscale comparison.
    //    Pixels: pure red, pure green, pure blue, a mixed tone.
    {
        let mut e = writer("rgb8.png", 4, 1);
        e.set_color(ColorType::Rgb);
        e.set_depth(BitDepth::Eight);
        let mut w = e.write_header().unwrap();
        w.write_image_data(&[255, 0, 0, 0, 255, 0, 0, 0, 255, 123, 200, 50])
            .unwrap();
    }

    // 2) Same RGB pixels but tagged sRGB, so libpng's rgb_to_gray linearizes.
    {
        let mut e = writer("rgb8_srgb.png", 4, 1);
        e.set_color(ColorType::Rgb);
        e.set_depth(BitDepth::Eight);
        e.set_source_srgb(SrgbRenderingIntent::Perceptual);
        let mut w = e.write_header().unwrap();
        w.write_image_data(&[255, 0, 0, 0, 255, 0, 0, 0, 255, 123, 200, 50])
            .unwrap();
    }

    // 3) 16-bit grayscale ramp chosen to expose strip(truncate) vs scale(round).
    //    For value V: strip = V>>8 ; scale = round(V*255/65535) = round(V/257).
    //    200 -> strip 0  / scale 1   (differ)
    //    130 -> strip 0  / scale 1   (differ)
    //    65280-> strip 255/ scale 254 (differ)
    //    Others agree.
    {
        let vals: [u16; 8] = [0, 130, 200, 257, 258, 32768, 65280, 65535];
        let mut e = writer("gray16.png", vals.len() as u32, 1);
        e.set_color(ColorType::Grayscale);
        e.set_depth(BitDepth::Sixteen);
        let mut w = e.write_header().unwrap();
        let bytes: Vec<u8> = vals.iter().flat_map(|v| v.to_be_bytes()).collect();
        w.write_image_data(&bytes).unwrap();
    }

    // 4) 16-bit RGB, one pixel whose channels each diverge under strip vs scale.
    {
        let chans: [u16; 6] = [200, 130, 65280, 257, 32768, 65535];
        let mut e = writer("rgb16.png", 2, 1);
        e.set_color(ColorType::Rgb);
        e.set_depth(BitDepth::Sixteen);
        let mut w = e.write_header().unwrap();
        let bytes: Vec<u8> = chans.iter().flat_map(|v| v.to_be_bytes()).collect();
        w.write_image_data(&bytes).unwrap();
    }

    // 5) Sub-byte grayscale ramps (1/2/4-bit), MSB-first packing.
    {
        // 1-bit, width 4: pixels 0,1,0,1 -> 0b0101_0000
        let mut e = writer("gray1.png", 4, 1);
        e.set_color(ColorType::Grayscale);
        e.set_depth(BitDepth::One);
        let mut w = e.write_header().unwrap();
        w.write_image_data(&[0b0101_0000]).unwrap();
    }
    {
        // 2-bit, width 4: pixels 0,1,2,3 -> 0b00_01_10_11
        let mut e = writer("gray2.png", 4, 1);
        e.set_color(ColorType::Grayscale);
        e.set_depth(BitDepth::Two);
        let mut w = e.write_header().unwrap();
        w.write_image_data(&[0b00_01_10_11]).unwrap();
    }
    {
        // 4-bit, width 4: pixels 0,5,10,15 -> bytes 0x05, 0xAF
        let mut e = writer("gray4.png", 4, 1);
        e.set_color(ColorType::Grayscale);
        e.set_depth(BitDepth::Four);
        let mut w = e.write_header().unwrap();
        w.write_image_data(&[0x05, 0xAF]).unwrap();
    }

    // 6) 8-bit grayscale + tRNS: gray value 128 is transparent.
    {
        let mut e = writer("gray8_trns.png", 4, 1);
        e.set_color(ColorType::Grayscale);
        e.set_depth(BitDepth::Eight);
        e.set_trns(vec![0x00, 0x80]); // gray level 128, big-endian 16-bit
        let mut w = e.write_header().unwrap();
        w.write_image_data(&[0, 128, 200, 255]).unwrap();
    }

    // 7) 4-bit grayscale + tRNS: gray index 5 is transparent. Tests the
    //    interaction between sub-byte up-scaling and tRNS matching.
    {
        let mut e = writer("gray4_trns.png", 4, 1);
        e.set_color(ColorType::Grayscale);
        e.set_depth(BitDepth::Four);
        e.set_trns(vec![0x00, 0x05]); // gray index 5
        let mut w = e.write_header().unwrap();
        // pixels 0,5,10,15
        w.write_image_data(&[0x05, 0xAF]).unwrap();
    }

    // 8) 8-bit RGB + tRNS: color (0,255,0) is transparent.
    {
        let mut e = writer("rgb8_trns.png", 4, 1);
        e.set_color(ColorType::Rgb);
        e.set_depth(BitDepth::Eight);
        e.set_trns(vec![0, 0, 0, 255, 0, 0]); // r=0,g=255,b=0 big-endian
        let mut w = e.write_header().unwrap();
        w.write_image_data(&[255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 255, 0])
            .unwrap();
    }

    // 9) Indexed + tRNS: palette of 4 colors, per-index alpha.
    {
        let mut e = writer("palette_trns.png", 4, 1);
        e.set_color(ColorType::Indexed);
        e.set_depth(BitDepth::Eight);
        e.set_palette(vec![
            255, 0, 0, // idx0 red
            0, 255, 0, // idx1 green
            0, 0, 255, // idx2 blue
            255, 255, 255, // idx3 white
        ]);
        e.set_trns(vec![0x00, 0x80, 0xFF]); // idx0 transparent, idx1 half, idx2 opaque, idx3 default opaque
        let mut w = e.write_header().unwrap();
        w.write_image_data(&[0, 1, 2, 3]).unwrap();
    }

    println!("fixtures written to ./fixtures");
}
