# sped

**Simplest PNG ESP32 Decoder** -- a minimal streaming PNG decoder for embedded systems.

277 lines of C. Decodes PNG images to RGB565 row-by-row via callback. Uses tinfl (from miniz) for DEFLATE decompression.

## Features

- Streaming row-by-row output via callback (no full-image buffer needed)
- Direct RGB565 output (native format for most embedded LCD displays)
- Supports all 8-bit color types: grayscale, RGB, RGBA, grayscale+alpha, indexed (palette)
- Palette transparency (tRNS chunk)
- All five PNG scanline filter types (None, Sub, Up, Average, Paeth)
- ~35 KB working memory (dominated by 32 KB DEFLATE dictionary)

## Limitations

- 8-bit depth only (no 16-bit channels)
- No interlacing (Adam7)
- No CRC verification
- Requires miniz/tinfl for DEFLATE (available in ESP-IDF via `esp_rom`, or from [miniz](https://github.com/richgel999/miniz))

## API

```c
#include "sped.h"

/* Get image dimensions without decoding */
sped_info_t info;
sped_info(png_data, png_len, &info);
printf("%ux%u\n", info.width, info.height);

/* Decode with row callback */
void my_row(int y, int w, const uint16_t *rgb565, void *user) {
    /* blit rgb565 to display at row y */
}
sped_decode(png_data, png_len, my_row, NULL);
```

Both functions take the entire PNG file in memory. `sped_decode` calls the callback once per row (y=0 is the top row). The `rgb565` buffer is reused between rows -- consume it immediately.

## Building

### ESP-IDF

Add to your component's `CMakeLists.txt`:

```cmake
idf_component_register(
    SRCS "main.c" "path/to/sped.c"
    INCLUDE_DIRS "." "path/to/sped"
)
```

miniz is available automatically via `esp_rom`.

### Desktop / other platforms

```cmake
add_executable(myapp main.c sped.c)
target_link_libraries(myapp miniz)
```

Or configure the include path:

```c
#define SPED_INFLATE_INCLUDE "my_miniz.h"
```

## Comparison

| Library | Lines | License | Streaming | RGB565 | Interlace | 16-bit | Palette | Needs zlib | RAM |
|---------|------:|---------|:---------:|:------:|:---------:|:------:|:-------:|:----------:|----:|
| **sped** | **277** | **MIT** | **yes** | **yes** | no | no | **yes** | miniz | **~35 KB** |
| pngle | ~936 | MIT | yes | no | yes | yes | yes | miniz | ~43 KB |
| PNGdec | ~570 | Apache-2.0 | yes | yes | no | yes | yes | bundled | ~48 KB |
| uPNG | ~1,362 | BSD | no | no | no | no | no | built-in | full image |
| picoPNG | ~500 | zlib | no | no | no | no | limited | built-in | full image |
| LodePNG | ~9,432 | zlib | no | no | yes | yes | yes | built-in | full image |
| stb_image | ~7,988 | PD/MIT | no | no | yes | yes | yes | built-in | full image |
| libspng | ~7,517 | BSD-2 | yes | no | yes | yes | yes | optional | varies |

sped is the smallest PNG decoder with streaming RGB565 output. It trades features (interlacing, 16-bit, CRC checks) for minimal code size and memory usage, making it ideal for resource-constrained microcontrollers where you just need to get an image onto an LCD.

## License

MIT -- see [LICENSE](LICENSE).
