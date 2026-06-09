// Decodes a PNG with libpng, applying the classic read transforms a C++ caller
// would reach for, then emits logical samples in the same normalized format as
// the Rust harness so the two can be diffed directly.
//
// Flags (additive):
//   --expand        png_set_palette_to_rgb + expand_gray_1_2_4_to_8 + tRNS_to_alpha
//   --rgb-to-gray   png_set_rgb_to_gray (default coefficients)
//   --strip16       png_set_strip_16  (truncate: keep high byte)
//   --scale16       png_set_scale_16  (round: (V*255)/65535)
//   --gamma         configure sRGB gamma so rgb_to_gray works in linear light

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <png.h>

namespace {

struct Options {
  bool expand = false;
  bool rgb_to_gray = false;
  bool strip16 = false;
  bool scale16 = false;
  bool gamma = false;
};

const char* color_name(int color_type) {
  switch (color_type) {
    case PNG_COLOR_TYPE_GRAY:
      return "Gray";
    case PNG_COLOR_TYPE_GRAY_ALPHA:
      return "GrayA";
    case PNG_COLOR_TYPE_RGB:
      return "Rgb";
    case PNG_COLOR_TYPE_RGB_ALPHA:
      return "Rgba";
    case PNG_COLOR_TYPE_PALETTE:
      return "Indexed";
    default:
      return "Unknown";
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: png_decode <file.png> [flags]\n");
    return 2;
  }
  const char* path = argv[1];
  Options opt;
  for (int i = 2; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--expand") opt.expand = true;
    else if (a == "--rgb-to-gray") opt.rgb_to_gray = true;
    else if (a == "--strip16") opt.strip16 = true;
    else if (a == "--scale16") opt.scale16 = true;
    else if (a == "--gamma") opt.gamma = true;
    else { std::fprintf(stderr, "unknown flag %s\n", a.c_str()); return 2; }
  }

  std::string fname = path;
  if (auto pos = fname.find_last_of('/'); pos != std::string::npos)
    fname = fname.substr(pos + 1);

  FILE* fp = std::fopen(path, "rb");
  if (!fp) { std::perror("fopen"); return 1; }

  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  png_infop info = png_create_info_struct(png);
  if (setjmp(png_jmpbuf(png))) {
    std::fprintf(stderr, "libpng error decoding %s\n", path);
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(fp);
    return 1;
  }
  png_init_io(png, fp);
  png_read_info(png, info);

  png_uint_32 w = 0, h = 0;
  int bit_depth = 0, color_type = 0;
  png_get_IHDR(png, info, &w, &h, &bit_depth, &color_type, nullptr, nullptr, nullptr);

  // Gamma: establish sRGB file gamma + a matching output so rgb_to_gray runs in
  // linear light (gamma_to_1 / gamma_from_1 get built).
  if (opt.gamma) {
    double file_gamma = 0.45455;
    if (!png_get_gAMA(png, info, &file_gamma)) {
      if (png_get_valid(png, info, PNG_INFO_sRGB)) file_gamma = 0.45455;
    }
    // Output in the same sRGB encoding (display exponent 1/0.45455) so the only
    // net effect on the weighting is that it happens in linear space.
    png_set_gamma(png, 1.0 / 0.45455, file_gamma);
  }

  if (opt.rgb_to_gray) {
    // -1 keeps libpng's default Rec.709-ish coefficients (6968/23434/2366).
    png_set_rgb_to_gray_fixed(png, PNG_ERROR_ACTION_NONE, -1, -1);
  }

  if (opt.expand) {
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
      png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
  }

  if (opt.strip16) png_set_strip_16(png);
  if (opt.scale16) png_set_scale_16(png);

  png_read_update_info(png, info);

  // Re-query the post-transform layout.
  int out_bit_depth = png_get_bit_depth(png, info);
  int out_color_type = png_get_color_type(png, info);
  int channels = png_get_channels(png, info);
  size_t rowbytes = png_get_rowbytes(png, info);

  std::vector<png_byte> image(rowbytes * h);
  std::vector<png_bytep> rows(h);
  for (png_uint_32 y = 0; y < h; ++y) rows[y] = image.data() + y * rowbytes;
  png_read_image(png, rows.data());
  png_read_end(png, nullptr);

  // Emit logical samples (combine big-endian byte pairs for 16-bit).
  std::vector<long> samples;
  for (png_uint_32 y = 0; y < h; ++y) {
    png_bytep row = rows[y];
    size_t nsamp = static_cast<size_t>(w) * channels;
    if (out_bit_depth == 16) {
      for (size_t i = 0; i < nsamp; ++i) {
        long v = (static_cast<long>(row[2 * i]) << 8) | row[2 * i + 1];
        samples.push_back(v);
      }
    } else {
      for (size_t i = 0; i < nsamp; ++i) samples.push_back(row[i]);
    }
  }

  std::printf("# lib=libpng file=%s color=%s depth=%d w=%u h=%u channels=%d\n",
              fname.c_str(), color_name(out_color_type), out_bit_depth, w, h, channels);
  std::printf("samples:");
  for (long v : samples) std::printf(" %ld", v);
  std::printf("\n");

  png_destroy_read_struct(&png, &info, nullptr);
  std::fclose(fp);
  return 0;
}
