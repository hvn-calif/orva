#!/usr/bin/env bash
# Runs the image-crate vs libpng comparison matrix over the generated fixtures.
set -u
cd "$(dirname "$0")" || exit 1

RUST=./rust_tools/target/release/rust_decode
PNGC=./cpp/png_decode
FIX=fixtures

sect() { printf '\n========================================================\n%s\n========================================================\n' "$1"; }
run()  { printf '  $ %s\n' "$*"; "$@" | sed 's/^/      /'; }

sect "OPERATION A: COLOR -> GRAYSCALE"
echo "[A1] rgb8.png  (no gamma chunk)"
run "$RUST" "$FIX/rgb8.png" --luma
run "$PNGC" "$FIX/rgb8.png" --rgb-to-gray

echo
echo "[A2] rgb8_srgb.png  (sRGB-tagged): image vs libpng-no-gamma vs libpng-linear"
run "$RUST" "$FIX/rgb8_srgb.png" --luma
run "$PNGC" "$FIX/rgb8_srgb.png" --rgb-to-gray
run "$PNGC" "$FIX/rgb8_srgb.png" --rgb-to-gray --gamma

sect "OPERATION B: tRNS -> ALPHA"
for f in gray8_trns rgb8_trns palette_trns gray4_trns; do
  echo "[B] $f.png"
  run "$RUST" "$FIX/$f.png"
  run "$PNGC" "$FIX/$f.png" --expand
  echo
done

sect "OPERATION C1: SUB-BYTE -> 8-BIT UPSCALING (1/2/4 -> 8)"
for f in gray1 gray2 gray4; do
  echo "[C1] $f.png"
  run "$RUST" "$FIX/$f.png"
  run "$PNGC" "$FIX/$f.png" --expand
  echo
done

sect "OPERATION C2: 16-BIT -> 8-BIT DOWNSCALING"
echo "[C2] gray16.png  (values: 0 130 200 257 258 32768 65280 65535)"
echo "    pixel_bridge keeps 16-bit (no reduction):"
run "$RUST" "$FIX/gray16.png"
echo "    libpng --strip16 (truncate, keep high byte):"
run "$PNGC" "$FIX/gray16.png" --strip16
echo "    libpng --scale16 (round (V*255)/65535):"
run "$PNGC" "$FIX/gray16.png" --scale16
echo
echo "[C2] rgb16.png  (channels: 200 130 65280 | 257 32768 65535)"
run "$RUST" "$FIX/rgb16.png"
run "$PNGC" "$FIX/rgb16.png" --strip16
run "$PNGC" "$FIX/rgb16.png" --scale16
