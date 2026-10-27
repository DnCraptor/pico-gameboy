/*
 * Linux counterpart of drivers/hdmi/hdmi.c: the graphics_* API and the
 * scanline composer.
 *
 * On the RP2350 the DMA IRQ builds each scanline as 320 palette-index
 * samples, which the PIO/TMDS path doubles to 640 pixels.  2x and the text
 * mode change the samples on every second physical line; 3x, the stretched
 * picture and the gray lines need single physical lines.  Here every one of
 * the 480 physical lines is built with the same rules (scale, gray lines,
 * backplane, OSD bar, text mode) and expanded through the palette into a
 * 640x480 XRGB8888 frame.
 */
#include "graphics.h"

#include <string.h>

static const uint8_t textmode_palette[16] = {
    200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215
};

//активный видеорежим
static volatile enum graphics_mode_t graphics_mode = GRAPHICSMODE_DEFAULT;

//буфер палитры 256 цветов в формате R8G8B8
static uint32_t palette[256];

#define SCREEN_WIDTH (320)
#define SCREEN_HEIGHT (240)
#define LINES (SCREEN_HEIGHT * 2)

//графический буфер
static uint8_t* volatile graphics_buffer = NULL;
static volatile int graphics_buffer_width = 0;
static volatile int graphics_buffer_height = 0;
static volatile int graphics_buffer_shift_x = 0;
static volatile int graphics_buffer_shift_y = 0;
static volatile enum graphics_scale_t graphics_scale = GRAPHICS_SCALE_2X;
volatile uint32_t graphics_frame_count = 0;

#define OVERLAY_BAR_Y 228
#define OVERLAY_TEXT_Y 230
#define OVERLAY_TEXT_MAX 52
static char overlay_text[2][OVERLAY_TEXT_MAX + 1];
static uint8_t overlay_text_len[2];
static volatile uint8_t overlay_text_index = 0;
static volatile bool overlay_enabled = false;

//текстовый буфер
uint8_t* text_buffer = NULL;

/* gray lines: only the horizontal ones (a sample is 2 pixels wide); the grey
 * is a colour of the text palette: black, dark grey, light grey, white */
static volatile uint8_t gray_lines = 0;
static volatile uint8_t gray_slot = 208;
static const uint8_t gray_slots[4] = { 200, 208, 207, 215 };

static inline uint8_t screen_color(const uint8_t c) {
    return ((c & 0xf0) == 0xf0) ? 255 : c;
}

/* The state one frame is composed with: the emulation thread may change
 * the buffer, mode or scale at any moment, the frame uses one consistent
 * set (the RP2350 driver reads them per line). */
struct frame_state {
    enum graphics_mode_t mode;
    enum graphics_scale_t scale;
    const uint8_t* buffer;
    int width, height, shift_x, shift_y;
    const uint8_t* bp;          // backplane cells, NULL - none
    bool overlay;
    bool gray_h;
    uint8_t gray;
    int stretch_k;              // stretch: whole samples per pixel
    int16_t row[LINES];         // 3x / stretch: buffer row of every physical line, -1 - none
};

static void rows_update(struct frame_state* s) {
    const int h = s->height;
    const int k = s->scale == GRAPHICS_SCALE_3X ? 3 : 2;
    const int top = (LINES - k * h) / 2;
    for (int t = 0; t < LINES; t++) {
        const int r = t - top;
        s->row[t] = s->scale == GRAPHICS_SCALE_STRETCH ? (int16_t)(t * h / LINES)
                                                       : (int16_t)(r < 0 || r >= k * h ? -1 : r / k);
    }
}

/* the OSD bar: the bottom 24 physical lines, over any scale */
static void overlay_line(const int t, uint8_t* out) {
    const int y = t >> 1;
    memset(out, GRAPHICS_OSD_BG, SCREEN_WIDTH);
    if (y < OVERLAY_TEXT_Y || y >= OVERLAY_TEXT_Y + 8) return;
    const uint8_t index = overlay_text_index;
    const char* const str = overlay_text[index];
    const int len = overlay_text_len[index];
    const int x0 = (SCREEN_WIDTH - len * 6) / 2;
    for (int c = 0; c < len; ++c) {
        uint8_t bits = font_6x8[(uint8_t)str[c] * 8 + (y - OVERLAY_TEXT_Y)];
        for (int x = 0; x < 6; ++x, bits >>= 1)
            if (bits & 1) out[x0 + c * 6 + x] = GRAPHICS_OSD_FG;
    }
}

/* 3x, stretch, 2x with gray lines: one physical line t */
static void scaled_line(const struct frame_state* s, const int t, uint8_t* out) {
    const uint8_t* const bp = s->bp ? s->bp + (t >> 1) * BACKPLANE_W : NULL;
    const int scaled_row = s->row[t];
    if (scaled_row < 0) {
        if (bp) memcpy(out, bp, SCREEN_WIDTH);
        else memset(out, 255, SCREEN_WIDTH);
        return;
    }
    // the last physical line of a buffer row: grey
    const bool gray_row = s->gray_h && (t + 1 >= LINES || s->row[t + 1] != scaled_row);
    const uint8_t* src = &s->buffer[scaled_row * s->width];
    const int w = s->width;
    // 3x: 1.5 output samples per pixel (each sample is 2 physical pixels), 2x: 1
    const int out_w = s->scale == GRAPHICS_SCALE_3X ? w * 3 / 2
                    : s->scale == GRAPHICS_SCALE_2X ? w : SCREEN_WIDTH;
    const int left = (SCREEN_WIDTH - out_w) / 2;
    uint8_t* const line = out;
    if (bp) memcpy(out, bp, left);
    else memset(out, 255, left);
    out += left;
    if (gray_row) {
        memset(out, s->gray, out_w);
        out += out_w;
    }
    else if (s->scale == GRAPHICS_SCALE_2X) {
        for (int x = 0; x < w; x++)
            *out++ = screen_color(src[x]);
    }
    else if (s->scale == GRAPHICS_SCALE_3X) {
        for (int x = 0; x + 1 < w; x += 2) {
            const uint8_t a = screen_color(src[x]), b = screen_color(src[x + 1]);
            *out++ = a;
            *out++ = a;
            *out++ = b;
        }
    }
    else {
        const int k = s->stretch_k;
        for (int x = 0; x < w; x++) {
            memset(out, screen_color(src[x]), k);
            out += k;
        }
    }
    const int rest = (int)(line + SCREEN_WIDTH - out);
    if (rest <= 0) return;
    if (bp) memcpy(out, bp + (out - line), rest);
    else memset(out, 255, rest);
}

/* 2x without gray lines: logical line y (physical lines 2y, 2y + 1) */
static void graphics_line(const struct frame_state* s, const int y, uint8_t* out) {
    const uint8_t* const bp = s->bp && s->mode == GRAPHICSMODE_DEFAULT ? s->bp + y * BACKPLANE_W : NULL;
    const int x0 = s->shift_x, w = s->width;
    const int r = y - s->shift_y;
    if (bp) {
        // 2x with the backplane: the picture at its usual place
        if (r < 0 || r >= s->height || x0 < 0 || x0 + w > SCREEN_WIDTH) {
            memcpy(out, bp, SCREEN_WIDTH);
            return;
        }
        memcpy(out, bp, x0);
        const uint8_t* src = &s->buffer[r * w];
        uint8_t* o = out + x0;
        for (int x = 0; x < w; x++)
            *o++ = screen_color(src[x]);
        memcpy(o, bp + x0 + w, SCREEN_WIDTH - x0 - w);
        return;
    }
    //заполняем пространство сверху и снизу графического буфера
    if (r < 0 || r >= s->height || x0 >= SCREEN_WIDTH || x0 + w < 0) {
        memset(out, 255, SCREEN_WIDTH);
        return;
    }
    uint8_t* const end = out + SCREEN_WIDTH;
    //рисуем пространство слева от буфера
    if (x0 > 0) {
        memset(out, 255, x0);
        out += x0;
    }
    //рисуем сам видеобуфер+пространство справа
    const uint8_t* src = &s->buffer[r * w];
    const uint8_t* const src_end = src + w;
    if (x0 < 0) src -= x0;
    while (out < end && src < src_end)
        *out++ = screen_color(*src++);
    if (out < end)
        memset(out, 255, end - out);
}

/* text mode: logical line y */
static void text_line(const int y, uint8_t* out) {
    if (!text_buffer) {
        memset(out, 255, SCREEN_WIDTH);
        return;
    }
    *out++ = 255;
    for (int x = 0; x < TEXTMODE_COLS; x++) {
        const unsigned offset = (y / 8) * (TEXTMODE_COLS * 2) + x * 2;
        const uint8_t c = text_buffer[offset];
        const uint8_t colorIndex = text_buffer[offset + 1];
        uint8_t glyph_row = font_6x8[c * 8 + y % 8];
        for (int bit = 6; bit--;) {
            *out++ = glyph_row & 1
                         ? textmode_palette[colorIndex & 0xf] //цвет шрифта
                         : textmode_palette[colorIndex >> 4]; //цвет фона
            glyph_row >>= 1;
        }
    }
    *out = 255;
}

static void expand_line(uint32_t* dst, const uint8_t* line) {
    for (int x = 0; x < SCREEN_WIDTH; x++) {
        const uint32_t c = palette[line[x]];
        *dst++ = c;
        *dst++ = c;
    }
}

void hdmi_linux_compose(uint32_t* frame) {
    static struct frame_state s;
    s.mode = graphics_mode;
    s.scale = graphics_scale;
    s.buffer = graphics_buffer;
    s.width = graphics_buffer_width;
    s.height = graphics_buffer_height;
    s.shift_x = graphics_buffer_shift_x;
    s.shift_y = graphics_buffer_shift_y;
    s.bp = backplane_cells;
    s.overlay = overlay_enabled && s.mode == GRAPHICSMODE_DEFAULT;
    s.gray_h = s.scale != GRAPHICS_SCALE_STRETCH && (gray_lines & GRAPHICS_GRAY_HORIZONTAL);
    s.gray = gray_slot;
    s.stretch_k = s.width > 0 ? SCREEN_WIDTH / s.width : 2;
    const bool graphics = s.mode == GRAPHICSMODE_DEFAULT || s.mode == VGA_320x240x256;
    const bool text = s.mode == TEXTMODE_DEFAULT || s.mode == TEXTMODE_53x30;
    const bool per_line = s.mode == GRAPHICSMODE_DEFAULT && s.buffer &&
                          (s.scale != GRAPHICS_SCALE_2X || s.gray_h);
    if (per_line) rows_update(&s);

    uint8_t line[SCREEN_WIDTH];
    for (int t = 0; t < LINES; t++) {
        uint32_t* const row = frame + (size_t)t * HDMI_FRAME_WIDTH;
        const bool overlay_row = s.overlay && t >= OVERLAY_BAR_Y * 2;
        if (!per_line && !overlay_row && (t & 1)) {
            // the samples change on every second physical line
            memcpy(row, row - HDMI_FRAME_WIDTH, HDMI_FRAME_WIDTH * sizeof(uint32_t));
            continue;
        }
        if (!s.buffer) memset(line, 255, SCREEN_WIDTH);
        else if (overlay_row) overlay_line(t, line);
        else if (per_line) scaled_line(&s, t, line);
        else if (graphics) graphics_line(&s, t >> 1, line);
        else if (text) text_line(t >> 1, line);
        else memset(line, 255, SCREEN_WIDTH);
        expand_line(row, line);
    }
    graphics_frame_count++; // the vertical blank begins
}

void graphics_set_overlay(const char* text, bool enabled) {
    if (enabled && text) {
        const uint8_t next = overlay_text_index ^ 1u;
        strncpy(overlay_text[next], text, OVERLAY_TEXT_MAX);
        overlay_text[next][OVERLAY_TEXT_MAX] = '\0';
        uint8_t len = 0;
        while (len < OVERLAY_TEXT_MAX && overlay_text[next][len]) ++len;
        overlay_text_len[next] = len;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        overlay_text_index = next;
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    overlay_enabled = enabled;
}

//выбор видеорежима
void graphics_set_mode(enum graphics_mode_t mode) {
    graphics_mode = mode;
    clrScr(0);
}

void graphics_set_palette(uint8_t i, uint32_t color888) {
    palette[i] = color888 & 0x00ffffff;
}

void graphics_set_buffer(uint8_t* buffer, uint16_t width, uint16_t height) {
    graphics_buffer_width = width;
    graphics_buffer_height = height;
    graphics_buffer = buffer;
}

void graphics_init(void) {
    graphics_set_palette(200, RGB888(0x00, 0x00, 0x00)); //black
    graphics_set_palette(201, RGB888(0x00, 0x00, 0xC4)); //blue
    graphics_set_palette(202, RGB888(0x00, 0xC4, 0x00)); //green
    graphics_set_palette(203, RGB888(0x00, 0xC4, 0xC4)); //cyan
    graphics_set_palette(204, RGB888(0xC4, 0x00, 0x00)); //red
    graphics_set_palette(205, RGB888(0xC4, 0x00, 0xC4)); //magenta
    graphics_set_palette(206, RGB888(0xC4, 0x7E, 0x00)); //brown
    graphics_set_palette(207, RGB888(0xC4, 0xC4, 0xC4)); //light gray
    graphics_set_palette(208, RGB888(0x4E, 0x4E, 0x4E)); //dark gray
    graphics_set_palette(209, RGB888(0x4E, 0x4E, 0xDC)); //light blue
    graphics_set_palette(210, RGB888(0x4E, 0xDC, 0x4E)); //light green
    graphics_set_palette(211, RGB888(0x4E, 0xF3, 0xF3)); //light cyan
    graphics_set_palette(212, RGB888(0xDC, 0x4E, 0x4E)); //light red
    graphics_set_palette(213, RGB888(0xF3, 0x4E, 0xF3)); //light magenta
    graphics_set_palette(214, RGB888(0xF3, 0xF3, 0x4E)); //yellow
    graphics_set_palette(215, RGB888(0xFF, 0xFF, 0xFF)); //white
}

void graphics_set_bgcolor(uint32_t color888) //определяем зарезервированный цвет в палитре
{
    graphics_set_palette(255, color888);
}

void graphics_set_offset(int x, int y) {
    graphics_buffer_shift_x = x;
    graphics_buffer_shift_y = y;
}

void graphics_set_scale(enum graphics_scale_t scale) {
    graphics_scale = scale;
}

void graphics_set_gray_lines(const uint8_t lines, const uint8_t level) {
    gray_slot = gray_slots[level & 3u];
    gray_lines = lines & GRAPHICS_GRAY_HORIZONTAL; // vertical: below the sample size
}

void graphics_set_textbuffer(uint8_t* buffer) {
    text_buffer = buffer;
}

void clrScr(const uint8_t color) {
    if (text_buffer)
        memset(text_buffer, color, TEXTMODE_COLS * TEXTMODE_ROWS * 2);
}
