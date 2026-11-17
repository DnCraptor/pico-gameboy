#include "graphics.h"
#include <stdlib.h>
#include <string.h>

/* The scanline code reads these cells from RAM. */
uint8_t* volatile backplane_cells = NULL;

#if (VGA || HDMI || SOFTTV) && PICO_RP2350
struct backplane_rle {
    const uint8_t* bytes;
    size_t size;
};

struct builtin_backplane {
    uint32_t rgb[BACKPLANE_LOADED_COLORS];
    struct backplane_rle scale[2];
};

#include "backplane_skins.h"

_Static_assert(sizeof backplane_skins / sizeof backplane_skins[0] == BACKPLANE_CUSTOM,
               "Each built-in skin needs a GBBP asset");

static uint8_t* cells;
static int unpacked_scale = -1;
static int unpacked_skin = -1;

static uint8_t backplane_slot(const unsigned color) {
    return 192 + color + (color >= 8 ? 16 : 0);
}

static void unpack(const struct backplane_rle* rle) {
    uint8_t* out = cells;
    uint8_t* const end = cells + BACKPLANE_W * BACKPLANE_H;
    for (size_t i = 0; i + 1 < rle->size && out < end; i += 2) {
        size_t n = rle->bytes[i];
        if (n > (size_t)(end - out)) n = end - out;
        memset(out, rle->bytes[i + 1], n);
        out += n;
    }
}

/* Make the cells available for loading a custom GBBP from the SD card. */
uint8_t* backplane_buffer(void) {
    backplane_cells = NULL;
    if (!cells) cells = (uint8_t*)malloc(BACKPLANE_W * BACKPLANE_H);
    unpacked_skin = -1;
    return cells;
}

void backplane_show_loaded(const uint8_t rgb[BACKPLANE_LOADED_COLORS * 3]) {
    if (!cells) return;
    for (int i = 0; i < BACKPLANE_W * BACKPLANE_H; i++)
        cells[i] = backplane_slot(cells[i] % BACKPLANE_LOADED_COLORS);
    for (int i = 0; i < BACKPLANE_LOADED_COLORS; i++)
        graphics_set_palette(backplane_slot(i),
                             (uint32_t)rgb[i * 3] << 16 | rgb[i * 3 + 1] << 8 | rgb[i * 3 + 2]);
    backplane_cells = cells;
}

void graphics_set_backplane(bool on, enum graphics_scale_t scale, enum backplane_skin_t skin) {
    backplane_cells = NULL;
    if (!on || scale == GRAPHICS_SCALE_STRETCH || (unsigned)skin >= BACKPLANE_CUSTOM) return;
    if (!cells && !backplane_buffer()) return;
    const struct builtin_backplane* selected = &backplane_skins[skin];
    if (unpacked_skin != (int)skin || unpacked_scale != (int)scale) {
        unpack(&selected->scale[scale == GRAPHICS_SCALE_3X]);
        unpacked_skin = skin;
        unpacked_scale = scale;
    }
    for (int i = 0; i < BACKPLANE_LOADED_COLORS; i++)
        graphics_set_palette(backplane_slot(i), selected->rgb[i]);
    backplane_cells = cells;
}
#endif
