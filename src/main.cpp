/**
 * Copyright (C) 2023 by Ilya Maslennikov <xrip@xrip.ru>
 * Copyright (C) 2022 by Mahyar Koshkouei <mk@deltabeard.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
// Peanut-GB emulator settings
#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define ENABLE_SDCARD 1
#define USE_PS2_KBD 1
#define USE_NESPAD 1


/* C Headers */
#include <cstdio>
#include <cstring>

/* RP2040 Headers */
#include "pico/runtime.h"
#include <hardware/sync.h>
#include <hardware/flash.h>
#include <hardware/timer.h>
#include <hardware/vreg.h>
#include <pico/stdio.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <sys/unistd.h>
#include <hardware/watchdog.h>

#include "audio.h"
#include "minigb_apu.h"

/* Project headers */
#include "hedley.h"
#include "peanut_gb.h"
#include "gbcolors.h"

/* Murmulator board */
#include "graphics.h"
#include "f_util.h"
#include "ff.h"


#include "nespad.h"

#include "ps2kbd_mrmltr.h"


/** Definition of ROM data
 * We're going to erase and reprogram a region 1Mb from the start of the flash
 * Once done, we can access this at XIP_BASE + 1Mb.
 * Game Boy DMG ROM size ranges from 32768 bytes (e.g. Tetris) to 1,048,576 bytes (e.g. Pokemod Red)
 */
#define HOME_DIR (char*)"\\GB"
extern char __flash_binary_end;
#define FLASH_TARGET_OFFSET (((((uintptr_t)&__flash_binary_end - XIP_BASE) / FLASH_SECTOR_SIZE) + 4) * FLASH_SECTOR_SIZE)
static const uint8_t* rom = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);

static uint8_t ram[32768];

semaphore vga_start_semaphore;

gb_s gb;

uint8_t SCREEN[LCD_HEIGHT][LCD_WIDTH];
static FATFS fs;

uint16_t stream[AUDIO_BUFFER_SIZE_BYTES];

#if TFT
#define RGB565_TO_RGB888(rgb565) (rgb565)
#else
#define RGB565_TO_RGB888(rgb565) ((((rgb565) & 0xF800) << 8) | (((rgb565) & 0x07E0) << 5) | (((rgb565) & 0x001F) << 3))
#endif

static inline uint32_t rgb565_to_rgb888(uint16_t c) { return ((uint32_t)(c & 0xF800) << 8) | ((uint32_t)(c & 0x07E0) << 5) | ((uint32_t)(c & 0x001F) << 3); }
static inline uint16_t rgb888_to_rgb565(uint32_t c) { return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F)); }
static inline uint32_t custom_to_driver_color(uint32_t c) {
#if TFT
    return rgb888_to_rgb565(c);
#else
    return c;
#endif
}

typedef uint32_t palette222_t[3][4];
static palette222_t palette;
static palette_t palette16; // Colour palette
static uint8_t manual_palette_selected = 0; // auto

static constexpr uint8_t PALETTE_CUSTOM = NUMBER_OF_MANUAL_PALETTES;
static constexpr uint8_t PALETTE_CUSTOM_PRESET = NUMBER_OF_MANUAL_PALETTES + 1;
static constexpr uint8_t PALETTE_CUSTOM_RANDOM = NUMBER_OF_MANUAL_PALETTES + 2;
static uint32_t custom_rgb[4] = { 0xD4FFFD, 0x139566, 0x106F4C, 0x000000 };
static uint8_t preset_rgb_index[4] = {0, 0, 0, 10};
static uint8_t hex_digit = 0;
static char current_rom_name[128] = {};
static bool game_palette_linked = false;
static bool game_palette_active = false;
static uint32_t game_palette_rgb[3][4] = {};

// Same curated LCD colour pools as Watara. Empty spreadsheet cells are omitted.
static const uint32_t preset_rgb0[] = {
    0xD4FFFD, 0xD4FFF3, 0xFFE9FA, 0xE9E9FF, 0xE9FAFF, 0xE9FFF4,
    0xF5FFE9, 0xFFF8E9, 0xFFEBE9, 0xD4FFDA, 0xEDFFD4
};
static const uint32_t preset_rgb1_cold[] = { 0x139566, 0x349BC0, 0x009999, 0x7296B6, 0xC0CBD5, 0xC3D5B5 };
static const uint32_t preset_rgb1_warm[] = { 0xE8AE74, 0xC57CDA };
static const uint32_t preset_rgb2_cold[] = { 0x106F4C, 0x1C5165, 0x006565, 0x496074 };
static const uint32_t preset_rgb2_warm[] = { 0xF79036, 0xBD5F00, 0xD58B41, 0xD8C835, 0xE98EA8 };
static const uint32_t preset_rgb3[] = {
    0x6C6800, 0x6B0400, 0x366C00, 0x006C48, 0x00686C, 0x00326C,
    0x04006C, 0x44006C, 0x6C0044, 0x6C0004, 0x000000
};
static constexpr uint8_t RGB1_COLD_COUNT = count_of(preset_rgb1_cold);
static constexpr uint8_t RGB1_COUNT = count_of(preset_rgb1_cold) + count_of(preset_rgb1_warm);
static constexpr uint8_t RGB2_COLD_COUNT = count_of(preset_rgb2_cold);
static constexpr uint8_t RGB2_COUNT = count_of(preset_rgb2_cold) + count_of(preset_rgb2_warm);

static uint32_t preset_rgb1_at(uint8_t i) { return i < RGB1_COLD_COUNT ? preset_rgb1_cold[i] : preset_rgb1_warm[i - RGB1_COLD_COUNT]; }
static uint32_t preset_rgb2_at(uint8_t i) { return i < RGB2_COLD_COUNT ? preset_rgb2_cold[i] : preset_rgb2_warm[i - RGB2_COLD_COUNT]; }
static uint32_t palette_random_state = 0x6D2B79F5u;
static uint32_t palette_random_next() { uint32_t x=palette_random_state; x^=x<<13; x^=x>>17; x^=x<<5; return palette_random_state=x; }
static void randomize_custom_palette() {
    uint64_t t=time_us_64(); palette_random_state ^= (uint32_t)t ^ (uint32_t)(t>>32);
    custom_rgb[0]=preset_rgb0[palette_random_next()%count_of(preset_rgb0)];
    uint8_t i1=palette_random_next()%RGB1_COUNT; custom_rgb[1]=preset_rgb1_at(i1);
    custom_rgb[2]=(i1<RGB1_COLD_COUNT) ? preset_rgb2_cold[palette_random_next()%count_of(preset_rgb2_cold)] : preset_rgb2_warm[palette_random_next()%count_of(preset_rgb2_warm)];
    custom_rgb[3]=preset_rgb3[palette_random_next()%count_of(preset_rgb3)];
}

struct input_bits_t {
    bool a: true;
    bool b: true;
    bool select: true;
    bool start: true;
    bool right: true;
    bool left: true;
    bool up: true;
    bool down: true;
};

static uint8_t swap_ab = 0;
static input_bits_t keyboard = { false, false, false, false, false, false, false, false }; //Keyboard
static input_bits_t gamepad_bits = { false, false, false, false, false, false, false, false }; //Joypad
//-----------------------------------------------------------------------------

void nespad_tick() {
    nespad_read();

    if (swap_ab) {
        gamepad_bits.b = keyboard.a || (nespad_state & DPAD_A) != 0;
        gamepad_bits.a = keyboard.b || (nespad_state & DPAD_B) != 0;
    } else {
        gamepad_bits.a = keyboard.a || (nespad_state & DPAD_A) != 0;
        gamepad_bits.b = keyboard.b || (nespad_state & DPAD_B) != 0;
    }
    gamepad_bits.select = keyboard.select || (nespad_state & DPAD_SELECT) != 0;
    gamepad_bits.start = keyboard.start || (nespad_state & DPAD_START) != 0;
    gamepad_bits.up = keyboard.up || (nespad_state & DPAD_UP) != 0;
    gamepad_bits.down = keyboard.down || (nespad_state & DPAD_DOWN) != 0;
    gamepad_bits.left = keyboard.left || (nespad_state & DPAD_LEFT) != 0;
    gamepad_bits.right = keyboard.right || (nespad_state & DPAD_RIGHT) != 0;
}

//-----------------------------------------------------------------------------


static bool isInReport(hid_keyboard_report_t const* report, const unsigned char keycode) {
    for (unsigned char i: report->keycode) {
        if (i == keycode) {
            return true;
        }
    }
    return false;
}

static volatile bool altPressed = false;
static volatile bool ctrlPressed = false;
static volatile uint8_t fxPressedV = 0;
static volatile bool pageUpPressed = false;
static volatile bool pageDownPressed = false;

void
__not_in_flash_func(process_kbd_report)(hid_keyboard_report_t const* report, hid_keyboard_report_t const* prev_report) {
    /*    printf("HID key report modifiers %2.2X report ", report->modifier);
        for (unsigned char i: report->keycode)
            printf("%2.2X", i);
        printf("\r\n");*/
    keyboard.start = isInReport(report, HID_KEY_ENTER) || isInReport(report, HID_KEY_KEYPAD_ENTER);
    keyboard.select = isInReport(report, HID_KEY_BACKSPACE) || isInReport(report, HID_KEY_ESCAPE) || isInReport(report, HID_KEY_KEYPAD_ADD);

    keyboard.a = isInReport(report, HID_KEY_Z) || isInReport(report, HID_KEY_O) || isInReport(report, HID_KEY_KEYPAD_0);
    keyboard.b = isInReport(report, HID_KEY_X) || isInReport(report, HID_KEY_P) || isInReport(report, HID_KEY_KEYPAD_DECIMAL);

    bool b7 = isInReport(report, HID_KEY_KEYPAD_7);
    bool b9 = isInReport(report, HID_KEY_KEYPAD_9);
    bool b1 = isInReport(report, HID_KEY_KEYPAD_1);
    bool b3 = isInReport(report, HID_KEY_KEYPAD_3);

    keyboard.up = b7 || b9 || isInReport(report, HID_KEY_ARROW_UP) || isInReport(report, HID_KEY_W) || isInReport(report, HID_KEY_KEYPAD_8);
    keyboard.down = b1 || b3 || isInReport(report, HID_KEY_ARROW_DOWN) || isInReport(report, HID_KEY_S) || isInReport(report, HID_KEY_KEYPAD_2) || isInReport(report, HID_KEY_KEYPAD_5);
    keyboard.left = b7 || b1 || isInReport(report, HID_KEY_ARROW_LEFT) || isInReport(report, HID_KEY_A) || isInReport(report, HID_KEY_KEYPAD_4);
    keyboard.right = b9 || b3 || isInReport(report, HID_KEY_ARROW_RIGHT)  || isInReport(report, HID_KEY_D) || isInReport(report, HID_KEY_KEYPAD_6);

    altPressed = isInReport(report, HID_KEY_ALT_LEFT) || isInReport(report, HID_KEY_ALT_RIGHT);
    ctrlPressed = isInReport(report, HID_KEY_CONTROL_LEFT) || isInReport(report, HID_KEY_CONTROL_RIGHT);
    if (isInReport(report, HID_KEY_PAGE_UP) && !isInReport(prev_report, HID_KEY_PAGE_UP))
        pageUpPressed = true;
    if (isInReport(report, HID_KEY_PAGE_DOWN) && !isInReport(prev_report, HID_KEY_PAGE_DOWN))
        pageDownPressed = true;
    
    if (altPressed && ctrlPressed && isInReport(report, HID_KEY_DELETE)) {
        watchdog_enable(10, true);
        while(true) {
            tight_loop_contents();
        }
    }
    if (ctrlPressed || altPressed) {
        uint8_t fxPressed = 0;
        if (isInReport(report, HID_KEY_F1)) fxPressed = 1;
        else if (isInReport(report, HID_KEY_F2)) fxPressed = 2;
        else if (isInReport(report, HID_KEY_F3)) fxPressed = 3;
        else if (isInReport(report, HID_KEY_F4)) fxPressed = 4;
        else if (isInReport(report, HID_KEY_F5)) fxPressed = 5;
        else if (isInReport(report, HID_KEY_F6)) fxPressed = 6;
        else if (isInReport(report, HID_KEY_F7)) fxPressed = 7;
        else if (isInReport(report, HID_KEY_F8)) fxPressed = 8;
        fxPressedV = fxPressed;
    }
}

Ps2Kbd_Mrmltr ps2kbd(
    pio1,
    PS2KBD_GPIO_FIRST,
    process_kbd_report
);

/**
 * Returns a byte from the ROM file at the given address.
 */
uint8_t __not_in_flash_func(gb_rom_read)(struct gb_s* gb, const uint_fast32_t addr) {
    return rom[addr];
}

/**
 * Returns a byte from the cartridge RAM at the given address.
 */
uint8_t __not_in_flash_func(gb_cart_ram_read)(struct gb_s* gb, const uint_fast32_t addr) {
    return ram[addr];
}

/**
 * Writes a given byte to the cartridge RAM at the given address.
 */
void __not_in_flash_func(gb_cart_ram_write)(struct gb_s* gb, const uint_fast32_t addr, const uint8_t val) {
    ram[addr] = val;
}

/**
 * Ignore all errors.
 */
void gb_error(struct gb_s* gb, const enum gb_error_e gb_err, const uint16_t addr) {
    const char* gb_err_str[4] = {
        "UNKNOWN",
        "INVALID OPCODE",
        "INVALID READ",
        "INVALID WRITE"
    };
    printf("Error %d occurred: %s at %04X\n.\n", gb_err, gb_err_str[gb_err], addr);
}

/* Renderer loop on Pico's second core */
void __time_critical_func(render_core)() {
    multicore_lockout_victim_init();
    graphics_init();

    const auto buffer = (uint8_t *)SCREEN;
    graphics_set_buffer(buffer, LCD_WIDTH, LCD_HEIGHT);
    graphics_set_textbuffer(buffer);
    graphics_set_bgcolor(0x000000);

#if VGA
    graphics_set_offset(60, 6);
#endif
#if HDMI | TV | SOFTTV
    graphics_set_offset(80, 48);
#endif

    graphics_set_flashmode(false, false);
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
    // clrScr(1);

    sem_acquire_blocking(&vga_start_semaphore);

    // 60 FPS loop
#define frame_tick (16666)
    uint64_t tick = time_us_64();
#ifdef TFT
    uint64_t last_renderer_tick = tick;
#endif
    uint64_t last_input_tick = tick;
    while (true) {
#ifdef TFT
        if (tick >= last_renderer_tick + frame_tick) {
            refresh_lcd();
            last_renderer_tick = tick;
        }
#endif
        if (tick >= last_input_tick + frame_tick * 1) {
            ps2kbd.tick();
            nespad_tick();
            last_input_tick = tick;
        }
        tick = time_us_64();


        // tuh_task();
        //hid_app_task();
        tight_loop_contents();
    }

    __unreachable();
}


/**
 * Draws scanline into framebuffer.
 */
void __always_inline lcd_draw_line(struct gb_s* gb, const uint8_t pixels[160], const uint_fast8_t y) {
    // memcpy((uint32_t *)SCREEN[y], (uint32_t *)pixels, 160);
    //         screen[y][x] = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
    if (gb->cgb.cgbMode) {
        memcpy((uint32_t *)SCREEN[y], (uint32_t *)pixels, 160);
    }
    else {
        for (unsigned int x = 0; x < LCD_WIDTH; x++)
            SCREEN[y][x] = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
    }
}

/**
 * Load a save file from the SD card
 */
void read_cart_ram_file(struct gb_s* gb) {
    char filename[24];
    uint_fast32_t save_size;
    UINT br;

    gb_get_rom_name(gb, filename);
    save_size = gb_get_save_size(gb);
    if (save_size > 0) {
        FIL fil;
        FRESULT fr = f_open(&fil, filename, FA_READ);
        if (fr == FR_OK) {
            f_read(&fil, ram, f_size(&fil), &br);
        }
        else {
            printf("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
        }

        fr = f_close(&fil);
        if (fr != FR_OK) {
            printf("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
        }
    }
    printf("I read_cart_ram_file(%s) COMPLETE (%u bytes)\n", filename, save_size);
}

/**
 * Write a save file to the SD card
 */
void write_cart_ram_file(struct gb_s* gb) {
    char filename[16];
    uint_fast32_t save_size;
    UINT bw;

    gb_get_rom_name(gb, filename);
    save_size = gb_get_save_size(gb);
    if (save_size > 0) {
        FIL fil;
        FRESULT fr = f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr == FR_OK) {
            f_write(&fil, ram, save_size, &bw);
        }
        else {
            printf("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
        }

        fr = f_close(&fil);
        if (fr != FR_OK) {
            printf("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
        }
    }
    printf("I write_cart_ram_file(%s) COMPLETE (%u bytes)\n", filename, save_size);
}


typedef struct __attribute__((__packed__)) {
    bool is_directory;
    bool is_executable;
    size_t size;
    char filename[79];
} file_item_t;

constexpr int max_files = 500;
static file_item_t fileItems[max_files];

int compareFileItems(const void* a, const void* b) {
    const auto* itemA = (file_item_t *)a;
    const auto* itemB = (file_item_t *)b;
    // Directories come first
    if (itemA->is_directory && !itemB->is_directory)
        return -1;
    if (!itemA->is_directory && itemB->is_directory)
        return 1;
    // Sort files alphabetically
    return strcmp(itemA->filename, itemB->filename);
}

bool isExecutable(const char pathname[255],const char *extensions) {
    char *pathCopy = strdup(pathname);
    const char* token = strrchr(pathCopy, '.');

    if (token == nullptr) {
        return false;
    }

    token++;

    while (token != NULL) {
        if (strstr(extensions, token) != NULL) {
            free(pathCopy);
            return true;
        }
        token = strtok(NULL, ",");
    }
    free(pathCopy);
    return false;
}

static void apply_dmg_palette() {
    if (game_palette_active && manual_palette_selected == PALETTE_CUSTOM) {
        for (int i=0;i<3;i++) for (int j=0;j<4;j++) { graphics_set_palette(i*4+j, custom_to_driver_color(game_palette_rgb[i][j])); palette[i][j]=i*4+j; }
        return;
    }
    if (manual_palette_selected == PALETTE_CUSTOM || manual_palette_selected == PALETTE_CUSTOM_PRESET || manual_palette_selected == PALETTE_CUSTOM_RANDOM) {
        for (int i=0;i<3;i++) for (int j=0;j<4;j++) { graphics_set_palette(i*4+j, custom_to_driver_color(custom_rgb[j])); palette[i][j]=i*4+j; }
        return;
    }
    if (manual_palette_selected > 0) manual_assign_palette(palette16, manual_palette_selected);
    else { char rom_title[16]; auto_assign_palette(palette16, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title)); }
    for (int i=0;i<3;i++) for (int j=0;j<4;j++) { graphics_set_palette(i*4+j, RGB565_TO_RGB888(palette16[i][j])); palette[i][j]=i*4+j; }
}

static void game_ini_path(char *out, size_t n) {
    char base[128]; strncpy(base,current_rom_name,sizeof(base)-1); base[sizeof(base)-1]=0;
    char *dot=strrchr(base,'.'); if(dot)*dot=0; snprintf(out,n,"/.config/gameboy/%s.ini",base);
}
static bool game_palette_read() {
    game_palette_active=false; game_palette_linked=false; if(!current_rom_name[0]) return false;
    char path[256]; game_ini_path(path,sizeof(path)); FIL f; if(f_open(&f,path,FA_READ)!=FR_OK) return false;
    char buf[512]={}; UINT br=0; f_read(&f,buf,sizeof(buf)-1,&br); f_close(&f); buf[br]=0;
    unsigned long v[12]; int n=sscanf(buf,
        "[palette]\\nbg0=%lx\\nbg1=%lx\\nbg2=%lx\\nbg3=%lx\\nobj10=%lx\\nobj11=%lx\\nobj12=%lx\\nobj13=%lx\\nobj20=%lx\\nobj21=%lx\\nobj22=%lx\\nobj23=%lx",
        &v[0],&v[1],&v[2],&v[3],&v[4],&v[5],&v[6],&v[7],&v[8],&v[9],&v[10],&v[11]);
    game_palette_linked=true; if(n!=12) return false;
    for(int i=0;i<3;i++) for(int j=0;j<4;j++) game_palette_rgb[i][j]=(uint32_t)v[i*4+j]&0xFFFFFF;
    for(int j=0;j<4;j++) custom_rgb[j]=game_palette_rgb[0][j];
    game_palette_active=true; manual_palette_selected=PALETTE_CUSTOM; return true;
}
static bool game_palette_write() {
    if(!current_rom_name[0]) return false; f_mkdir("/.config"); f_mkdir("/.config/gameboy");
    uint32_t c[3][4];
    if(game_palette_active && manual_palette_selected==PALETTE_CUSTOM) memcpy(c,game_palette_rgb,sizeof(c));
    else if(manual_palette_selected>=PALETTE_CUSTOM) for(int i=0;i<3;i++) for(int j=0;j<4;j++) c[i][j]=custom_rgb[j];
    else { palette_t p; if(manual_palette_selected) manual_assign_palette(p,manual_palette_selected); else { char t[16]; auto_assign_palette(p,gb_colour_hash(&gb),gb_get_rom_name(&gb,t)); } for(int i=0;i<3;i++) for(int j=0;j<4;j++) c[i][j]=rgb565_to_rgb888(p[i][j]); }
    char text[384]; int len=snprintf(text,sizeof(text),"[palette]\\nbg0=%06lX\\nbg1=%06lX\\nbg2=%06lX\\nbg3=%06lX\\nobj10=%06lX\\nobj11=%06lX\\nobj12=%06lX\\nobj13=%06lX\\nobj20=%06lX\\nobj21=%06lX\\nobj22=%06lX\\nobj23=%06lX\\n",
      (unsigned long)c[0][0],(unsigned long)c[0][1],(unsigned long)c[0][2],(unsigned long)c[0][3],(unsigned long)c[1][0],(unsigned long)c[1][1],(unsigned long)c[1][2],(unsigned long)c[1][3],(unsigned long)c[2][0],(unsigned long)c[2][1],(unsigned long)c[2][2],(unsigned long)c[2][3]);
    char path[256]; game_ini_path(path,sizeof(path)); FIL f; if(f_open(&f,path,FA_CREATE_ALWAYS|FA_WRITE)!=FR_OK)return false; UINT bw=0; FRESULT r=f_write(&f,text,len,&bw); FRESULT rc=f_close(&f); return r==FR_OK&&rc==FR_OK&&bw==(UINT)len;
}
static bool game_palette_unlink() { char p[256]; game_ini_path(p,sizeof(p)); FRESULT r=f_unlink(p); game_palette_active=false; return r==FR_OK||r==FR_NO_FILE; }

static bool demo_requested = false;
static bool demo_active = false;
static bool demo_advance_pending = false;
static uint64_t demo_game_started_at = 0;
static char demo_current_name[128] = {};
static const uint16_t demo_seconds[] = { 15, 30, 45, 60, 120, 180, 300, 600 };
static uint8_t demo_duration = 0;

bool __not_in_flash_func(filebrowser_loadfile)(const char pathname[256]) {
    UINT bytes_read = 0;
    FIL file;

    constexpr int window_y = (TEXTMODE_ROWS - 5) / 2;
    constexpr int window_x = (TEXTMODE_COLS - 43) / 2;
    const auto show_load_error = [&](const char *message) {
        draw_text(message, window_x + 1, window_y + 2, 13, 1);
        sleep_ms(demo_active ? 1500 : 5000);
    };

    draw_window("Loading ROM", window_x, window_y, 43, 5);

    /* Open the browser-selected path first; the original loader did not
       depend on a separate f_stat() path lookup. */
    if (FR_OK != f_open(&file, pathname, FA_READ)) {
        show_load_error("ERROR: ROM open failed!");
        return false;
    }

    const uint32_t load_size = f_size(&file);
    if (load_size == 0) {
        f_close(&file);
        show_load_error("ERROR: ROM is empty!");
        return false;
    }

    const uint32_t flash_capacity = PICO_FLASH_SIZE_BYTES - FLASH_TARGET_OFFSET;
    if (load_size > flash_capacity) {
        f_close(&file);
        show_load_error("ERROR: ROM too large! Canceled!!");
        return false;
    }

    draw_text("Loading...", window_x + 1, window_y + 2, 10, 1);

    uint32_t total_read = 0;
    bool flash_verify_failed = false;
    FRESULT read_result = FR_OK;
    multicore_lockout_start_blocking();
    uint32_t flash_target_offset = FLASH_TARGET_OFFSET;
    static uint8_t buffer[FLASH_SECTOR_SIZE] __aligned(4);

    do {
        memset(buffer, 0xff, sizeof(buffer));
        read_result = f_read(&file, buffer, sizeof(buffer), &bytes_read);
        total_read += bytes_read;

        if (read_result == FR_OK && bytes_read) {
            const uint8_t *flash_data = (const uint8_t *)(XIP_BASE + flash_target_offset);
            if (memcmp(flash_data, buffer, sizeof(buffer)) != 0) {
                const uint32_t ints = save_and_disable_interrupts();
                flash_range_erase(flash_target_offset, FLASH_SECTOR_SIZE);
                flash_range_program(flash_target_offset, buffer, FLASH_SECTOR_SIZE);
                restore_interrupts(ints);

                if (memcmp(flash_data, buffer, sizeof(buffer)) != 0) {
                    flash_verify_failed = true;
                    break;
                }
            }

            gpio_put(PICO_DEFAULT_LED_PIN, (flash_target_offset >> 13) & 1);
            flash_target_offset += FLASH_SECTOR_SIZE;
        }
    } while (read_result == FR_OK && bytes_read != 0);

    gpio_put(PICO_DEFAULT_LED_PIN, true);
    multicore_lockout_end_blocking();
    FRESULT close_result = f_close(&file);

    if (flash_verify_failed) {
        show_load_error("ERROR: Flash verify failed!");
        return false;
    }
    if (read_result != FR_OK || close_result != FR_OK || total_read != load_size) {
        show_load_error("ERROR: ROM load failed!");
        return false;
    }

    const char *bn = strrchr(pathname, '\\');
    if (!bn) bn = strrchr(pathname, '/');
    bn = bn ? bn + 1 : pathname;
    strncpy(current_rom_name, bn, sizeof(current_rom_name) - 1);
    current_rom_name[sizeof(current_rom_name) - 1] = 0;
    game_palette_linked = false;
    game_palette_active = false;
    return true;
}

static bool demo_load_next_rom(const char *after_name) {
    char after[128] = {};
    if (after_name && after_name[0]) {
        strncpy(after, after_name, sizeof(after) - 1);
    }

    /* Walk the root ROM directory in lexical order. Failed ROMs are skipped. */
    for (;;) {
        DIR dir;
        FILINFO info;
        if (FR_OK != f_opendir(&dir, HOME_DIR))
            return false;

        char best[128] = {};
        while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
            if (info.fattrib & AM_DIR)
                continue;
            if (!isExecutable(info.fname, "gbc,gb"))
                continue;
            if (after[0] && strcmp(info.fname, after) <= 0)
                continue;
            if (!best[0] || strcmp(info.fname, best) < 0) {
                strncpy(best, info.fname, sizeof(best) - 1);
            }
        }
        f_closedir(&dir);

        if (!best[0])
            return false;

        char pathname[256];
        snprintf(pathname, sizeof(pathname), "%s\\%s", HOME_DIR, best);
        if (!filebrowser_loadfile(pathname)) {
            strncpy(after, best, sizeof(after) - 1);
            continue;
        }

        strncpy(demo_current_name, best, sizeof(demo_current_name) - 1);
        demo_current_name[sizeof(demo_current_name) - 1] = 0;
        demo_game_started_at = time_us_64();
        return true;
    }
}

bool __not_in_flash_func(filebrowser)(const char pathname[256], const char executables[11]) {
    bool demo_debounce = false;
    bool debounce = true;
    char basepath[256];
    char tmp[TEXTMODE_COLS + 1];
    strcpy(basepath, pathname);
    constexpr int per_page = TEXTMODE_ROWS - 3;

    DIR dir;
    FILINFO fileInfo;

    while (true) {
        memset(fileItems, 0, sizeof(file_item_t) * max_files);
        int total_files = 0;

        snprintf(tmp, TEXTMODE_COLS, "SD:\\%s", basepath);
        draw_window(tmp, 0, 0, TEXTMODE_COLS, TEXTMODE_ROWS - 1);
        memset(tmp, ' ', TEXTMODE_COLS);


        draw_text(tmp, 0, 29, 0, 0);
        auto off = 0;
        draw_text("START", off, 29, 7, 0);
        off += 5;
        draw_text(" Run at cursor ", off, 29, 0, 3);
        off += 16;
        draw_text("SELECT", off, 29, 7, 0);
        off += 6;
        draw_text(" Run previous  ", off, 29, 0, 3);
#ifndef TFT
        off += 16;
        draw_text("ARROWS", off, 29, 7, 0);
        off += 6;
        draw_text(" Navigation    ", off, 29, 0, 3);
        off += 16;
        draw_text("A/F10", off, 29, 7, 0);
        off += 5;
        draw_text(" USB ", off, 29, 0, 3);
        off += 5;
        draw_text("B", off, 29, 7, 0);
        off += 1;
        draw_text(" Demo", off, 29, 0, 3);
#endif

        if (FR_OK != f_opendir(&dir, basepath)) {
            draw_text("Failed to open directory", 1, 1, 4, 0);
            while (true);
        }

        if (strlen(basepath) > 0) {
            strcpy(fileItems[total_files].filename, "..\0");
            fileItems[total_files].is_directory = true;
            fileItems[total_files].size = 0;
            total_files++;
        }

        while (f_readdir(&dir, &fileInfo) == FR_OK &&
               fileInfo.fname[0] != '\0' &&
               total_files < max_files
        ) {
            // Set the file item properties
            fileItems[total_files].is_directory = fileInfo.fattrib & AM_DIR;
            fileItems[total_files].size = fileInfo.fsize;
            fileItems[total_files].is_executable = isExecutable(fileInfo.fname, executables);
            strncpy(fileItems[total_files].filename, fileInfo.fname, 78);
            total_files++;
        }
        f_closedir(&dir);

        qsort(fileItems, total_files, sizeof(file_item_t), compareFileItems);

        if (total_files > max_files) {
            draw_text(" Too many files!! ", TEXTMODE_COLS - 17, 0, 12, 3);
        }

        int offset = 0;
        int current_item = 0;

        while (true) {
            sleep_ms(100);

            if (!debounce) {
                debounce = !(gamepad_bits.start);
            }

            if (!gamepad_bits.b)
                demo_debounce = true;
            if (demo_debounce && gamepad_bits.b) {
                demo_requested = true;
                return false;
            }

            // ESCAPE
            if (gamepad_bits.select) {
                return false;
            }

            if (gamepad_bits.down) {
                if (offset + (current_item + 1) < total_files) {
                    if (current_item + 1 < per_page) {
                        current_item++;
                    }
                    else {
                        offset++;
                    }
                }
            }

            if (gamepad_bits.up) {
                if (current_item > 0) {
                    current_item--;
                }
                else if (offset > 0) {
                    offset--;
                }
            }

            if (gamepad_bits.right) {
                offset += per_page;
                if (offset + (current_item + 1) > total_files) {
                    offset = total_files - (current_item + 1);
                }
            }

            if (gamepad_bits.left) {
                if (offset > per_page) {
                    offset -= per_page;
                }
                else {
                    offset = 0;
                    current_item = 0;
                }
            }

            constexpr int half_page = per_page / 2;
            if (pageDownPressed && total_files > 0) {
                pageDownPressed = false;
                int selected = offset + current_item + half_page;
                if (selected >= total_files) selected = total_files - 1;
                if (selected < offset + per_page) current_item = selected - offset;
                else { current_item = per_page - 1; offset = selected - current_item; }
            }
            if (pageUpPressed && total_files > 0) {
                pageUpPressed = false;
                int selected = offset + current_item - half_page;
                if (selected < 0) selected = 0;
                if (selected >= offset) current_item = selected - offset;
                else { current_item = 0; offset = selected; }
            }

            if (debounce && gamepad_bits.start) {
                auto file_at_cursor = fileItems[offset + current_item];

                if (file_at_cursor.is_directory) {
                    if (strcmp(file_at_cursor.filename, "..") == 0) {
                        const char* lastBackslash = strrchr(basepath, '\\');
                        if (lastBackslash != nullptr) {
                            const size_t length = lastBackslash - basepath;
                            basepath[length] = '\0';
                        }
                    }
                    else {
                        sprintf(basepath, "%s\\%s", basepath, file_at_cursor.filename);
                    }
                    debounce = false;
                    break;
                }

                if (file_at_cursor.is_executable) {
                    sprintf(tmp, "%s\\%s", basepath, file_at_cursor.filename);

                    if (filebrowser_loadfile(tmp))
                        return true;
                    debounce = false;
                }
            }

            for (int i = 0; i < per_page; i++) {
                uint8_t color = 11;
                uint8_t bg_color = 1;

                if (offset + i < max_files) {
                    const auto item = fileItems[offset + i];


                    if (i == current_item) {
                        color = 0;
                        bg_color = 3;
                        memset(tmp, 0xCD, TEXTMODE_COLS - 2);
                        tmp[TEXTMODE_COLS - 2] = '\0';
                        draw_text(tmp, 1, per_page + 1, 11, 1);
                        snprintf(tmp, TEXTMODE_COLS - 2, " Size: %iKb, File %lu of %i ", item.size / 1024,
                                 offset + i + 1,
                                 total_files);
                        draw_text(tmp, 2, per_page + 1, 14, 3);
                    }

                    const auto len = strlen(item.filename);
                    color = item.is_directory ? 15 : color;
                    color = item.is_executable ? 10 : color;
                    //color = strstr((char *)rom_filename, item.filename) != nullptr ? 13 : color;

                    memset(tmp, ' ', TEXTMODE_COLS - 2);
                    tmp[TEXTMODE_COLS - 2] = '\0';
                    memcpy(&tmp, item.filename, len < TEXTMODE_COLS - 2 ? len : TEXTMODE_COLS - 2);
                }
                else {
                    memset(tmp, ' ', TEXTMODE_COLS - 2);
                }
                draw_text(tmp, 1, i + 1, color, bg_color);
            }
        }
    }
}


bool restart = false;

enum menu_type_e {
    NONE,
    INT,
    TEXT,
    ARRAY,
    HEX,
    GAME_PALETTE_LINK,

    SAVE,
    LOAD,
    START_DEMO,
    ROM_SELECT,
    RETURN,
};

typedef bool (*menu_callback_t)();

typedef struct __attribute__((__packed__)) {
    const char* text;
    menu_type_e type;
    const void* value;
    menu_callback_t callback;
    uint8_t max_value;
    char value_list[16][20];
} MenuItem;

static int save_slot = 0;
static uint16_t frequencies[] = { 378, 396, 404, 408, 412, 416, 420, 424, 433 };
static uint8_t frequency_index = 0;

bool overclock() {
#if PICO_RP2350
    volatile uint32_t *qmi_m0_timing=(uint32_t *)0x400d000c;
    vreg_disable_voltage_limit();
    vreg_set_voltage(VREG_VOLTAGE_1_60);
    sleep_ms(10);
    *qmi_m0_timing = 0x60007204;
    set_sys_clock_khz(frequencies[frequency_index] * KHZ, false);
    *qmi_m0_timing = 0x60007303;
    return true;
#else
    hw_set_bits(&vreg_and_chip_reset_hw->vreg, VREG_AND_CHIP_RESET_VREG_VSEL_BITS);
    sleep_ms(33);
    return set_sys_clock_khz(frequencies[frequency_index] * KHZ, true);
#endif
}

static bool save() {
    char pathname[255];
    char filename[24];
    gb_get_rom_name(&gb, filename);

    if (save_slot) {
        sprintf(pathname, "%s\\%s_%d.save", HOME_DIR, filename, save_slot);
    }
    else {
        sprintf(pathname, "%s\\%s.save", HOME_DIR, filename);
    }

    FIL fd;
    f_open(&fd, pathname, FA_CREATE_ALWAYS | FA_WRITE);
    UINT bw;
    f_write(&fd, &gb, sizeof(gb), &bw);
    f_write(&fd, ram, sizeof(ram), &bw);
    f_close(&fd);

    return true;
}

static bool load() {
    char pathname[255];
    char filename[24];
    gb_get_rom_name(&gb, filename);

    if (save_slot) {
        sprintf(pathname, "GB\\%s_%d.save", filename, save_slot);
    }
    else {
        sprintf(pathname, "GB\\%s.save", filename);
    }

    FIL fd;
    f_open(&fd, pathname, FA_READ);
    UINT br;
    f_read(&fd, &gb, sizeof(gb), &br);
    f_read(&fd, ram, sizeof(ram), &br);
    f_close(&fd);
    return true;
}
#if SOFTTV
typedef struct tv_out_mode_t {
    // double color_freq;
    float color_index;
    COLOR_FREQ_t c_freq;
    enum graphics_mode_t mode_bpp;
    g_out_TV_t tv_system;
    NUM_TV_LINES_t N_lines;
    bool cb_sync_PI_shift_lines;
    bool cb_sync_PI_shift_half_frame;
} tv_out_mode_t;
extern tv_out_mode_t tv_out_mode;

bool color_mode=true;
bool toggle_color() {
    color_mode=!color_mode;
    if(color_mode) {
        tv_out_mode.color_index= 1.0f;
    } else {
        tv_out_mode.color_index= 0.0f;
    }

    return true;
}
#endif
const MenuItem menu_items[] = {
    //{ "Player 1: %s",        ARRAY, &player_1_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
    //{ "Player 2: %s",        ARRAY, &player_2_input, 2, { "Keyboard ", "Gamepad 1", "Gamepad 2" }},
    { "Swap AB <> BA: %s", ARRAY, &swap_ab,  nullptr, 1, {"NO ", "YES"}},
    { "Palette: %s ", ARRAY, &manual_palette_selected, nullptr, PALETTE_CUSTOM_RANDOM,
        { "0 - AUTO      ", "1 - yellow-red", "2 - orange    ", "3 - negative  ", "4 - dark green", "5 - red       ", "6 - pink      ", "7 - green     ", "8 - dark blue ", "9 - pastel    ", "10 - blue     ", "11 - yellow   ", "12 - DMG      ", "CUSTOM        ", "CUSTOM PRESET ", "CUSTOM RANDOM " } },
    { "Save for this game", GAME_PALETTE_LINK },
    { "RGB0: %06lXh", HEX, &custom_rgb[0], nullptr, 0 },
    { "RGB1: %06lXh", HEX, &custom_rgb[1], nullptr, 0 },
    { "RGB2: %06lXh", HEX, &custom_rgb[2], nullptr, 0 },
    { "RGB3: %06lXh", HEX, &custom_rgb[3], nullptr, 0 },
    { "Demo game time: %s", ARRAY, &demo_duration, nullptr, 7,
        { "15 sec", "30 sec", "45 sec", "1 min ", "2 min ", "3 min ", "5 min ", "10 min" } },
    { "Start Demo", START_DEMO },
    {},
    { "Save state: %i", INT, &save_slot, &save, 8 },
    { "Load state: %i", INT, &save_slot, &load, 8 },
#if SOFTTV
    { "" },
    { "TV system %s", ARRAY, &tv_out_mode.tv_system, nullptr, 1, { "PAL ", "NTSC" } },
    { "TV Lines %s", ARRAY, &tv_out_mode.N_lines, nullptr, 3, { "624", "625", "524", "525" } },
    { "Freq %s", ARRAY, &tv_out_mode.c_freq, nullptr, 1, { "3.579545", "4.433619" } },
    { "Colors: %s", ARRAY, &color_mode, &toggle_color, 1, { "NO ", "YES" } },
    { "Shift lines %s", ARRAY, &tv_out_mode.cb_sync_PI_shift_lines, nullptr, 1, { "NO ", "YES" } },
    { "Shift half frame %s", ARRAY, &tv_out_mode.cb_sync_PI_shift_half_frame, nullptr, 1, { "NO ", "YES" } },
#endif
    {},
{
    "Overclocking: %s MHz", ARRAY, &frequency_index, &overclock, count_of(frequencies) - 1,
    { "378", "396", "404", "408", "412", "416", "420", "424", "432" }
},
{ "Press START / Enter to apply", NONE },
    { "Reset to ROM select", ROM_SELECT },
    { "Return to game", RETURN }
};
#define MENU_ITEMS_NUMBER (sizeof(menu_items) / sizeof (MenuItem))

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version, swap_ab, palette, preset[4], color_mode, demo_duration;
    uint32_t rgb[4];
} gb_settings_t;
static constexpr uint32_t GB_CONF_MAGIC=0x47424346; static constexpr uint8_t GB_CONF_VERSION=2;
static void f_load_conf(void) {
    FIL f; bool loaded=false;
    if(f_open(&f,"/.config/gameboy/gameboy.conf",FA_READ)==FR_OK) {
        gb_settings_t c{}; UINT br=0; f_read(&f,&c,sizeof(c),&br); f_close(&f);
        if(br==sizeof(c)&&c.magic==GB_CONF_MAGIC&&c.version==GB_CONF_VERSION) {
            swap_ab=c.swap_ab; manual_palette_selected=c.palette<=PALETTE_CUSTOM_RANDOM?c.palette:0;
            memcpy(preset_rgb_index,c.preset,4); memcpy(custom_rgb,c.rgb,sizeof(custom_rgb));
#if SOFTTV
            color_mode=c.color_mode;
#endif
            demo_duration = c.demo_duration < count_of(demo_seconds) ? c.demo_duration : 0;
            preset_rgb_index[0]%=count_of(preset_rgb0); preset_rgb_index[1]%=RGB1_COUNT; preset_rgb_index[2]%=RGB2_COUNT; preset_rgb_index[3]%=count_of(preset_rgb3); loaded=true;
        }
    }
    if(!loaded && f_open(&f,"/GB/gb.conf",FA_READ)==FR_OK) { UINT br=0; f_read(&f,&swap_ab,1,&br); f_read(&f,&manual_palette_selected,1,&br); f_close(&f); if(manual_palette_selected>=NUMBER_OF_MANUAL_PALETTES) manual_palette_selected=0; }
}
static void f_save_conf(void) {
    f_mkdir("/.config"); f_mkdir("/.config/gameboy"); gb_settings_t c{}; c.magic=GB_CONF_MAGIC; c.version=GB_CONF_VERSION; c.swap_ab=swap_ab; c.palette=manual_palette_selected; c.demo_duration=demo_duration;
    memcpy(c.preset,preset_rgb_index,4); memcpy(c.rgb,custom_rgb,sizeof(custom_rgb));
#if SOFTTV
    c.color_mode=color_mode;
#else
    c.color_mode=1;
#endif
    FIL f; if(f_open(&f,"/.config/gameboy/gameboy.conf",FA_CREATE_ALWAYS|FA_WRITE)==FR_OK){UINT bw=0; f_write(&f,&c,sizeof(c),&bw); f_close(&f);}
}

void menu() {
    bool exit = false;
    graphics_set_mode(TEXTMODE_DEFAULT);
    char footer[TEXTMODE_COLS];
    snprintf(footer, TEXTMODE_COLS, ":: %s ::", PICO_PROGRAM_NAME);
    draw_text(footer, TEXTMODE_COLS / 2 - strlen(footer) / 2, 0, 11, 1);
    snprintf(footer, TEXTMODE_COLS, ":: %s build %s %s ::", PICO_PROGRAM_VERSION_STRING, __DATE__,
             __TIME__);
    draw_text(footer, TEXTMODE_COLS / 2 - strlen(footer) / 2, TEXTMODE_ROWS - 1, 11, 1);
    uint current_item = 0;
    uint8_t previous_palette = manual_palette_selected;
    bool hex_editing = false;

    while (!exit) {
        for (int i = 0; i < MENU_ITEMS_NUMBER; i++) {
            uint8_t y = i + (TEXTMODE_ROWS - MENU_ITEMS_NUMBER >> 1);
            uint8_t x = TEXTMODE_COLS / 2 - 10;
            uint8_t color = 0xFF;
            uint8_t bg_color = 0x00;
            if (current_item == i) {
                color = 0x01;
                bg_color = 0xFF;
            }
            const MenuItem* item = &menu_items[i];
            if (i == current_item) {
                switch (item->type) {
                    case HEX: {
                        int pidx=(int)((uint32_t*)item->value-custom_rgb);
                        if(manual_palette_selected==PALETTE_CUSTOM_PRESET) {
                            uint8_t maxv=pidx==0?count_of(preset_rgb0):pidx==1?RGB1_COUNT:pidx==2?RGB2_COUNT:count_of(preset_rgb3);
                            if(gamepad_bits.right) preset_rgb_index[pidx]=(preset_rgb_index[pidx]+1)%maxv;
                            if(gamepad_bits.left) preset_rgb_index[pidx]=(preset_rgb_index[pidx]+maxv-1)%maxv;
                            custom_rgb[0]=preset_rgb0[preset_rgb_index[0]]; custom_rgb[1]=preset_rgb1_at(preset_rgb_index[1]); custom_rgb[2]=preset_rgb2_at(preset_rgb_index[2]); custom_rgb[3]=preset_rgb3[preset_rgb_index[3]];
                            game_palette_active=false;
                        } else if(manual_palette_selected==PALETTE_CUSTOM) {
                            if(gamepad_bits.start) hex_editing=!hex_editing;
                            if(hex_editing) {
                                uint32_t step=1u<<((5-hex_digit)*4); if(gamepad_bits.up) custom_rgb[pidx]=(custom_rgb[pidx]+step)&0xFFFFFF; if(gamepad_bits.down) custom_rgb[pidx]=(custom_rgb[pidx]-step)&0xFFFFFF;
                                if(gamepad_bits.right) hex_digit=(hex_digit+1)%6; if(gamepad_bits.left) hex_digit=(hex_digit+5)%6; game_palette_active=false;
                            }
                        }
                        apply_dmg_palette(); break; }
                    case GAME_PALETTE_LINK:
                        if(gamepad_bits.start&&current_rom_name[0]) { if(game_palette_linked){if(game_palette_unlink())game_palette_linked=false;} else if(game_palette_write())game_palette_linked=true; }
                        break;
                    case INT:
                    case ARRAY:
                        if (item->max_value != 0) {
                            auto* value = (uint8_t *)item->value;
                            if (gamepad_bits.right && *value < item->max_value) {
                                (*value)++;
                            }
                            if (gamepad_bits.left && *value > 0) {
                                (*value)--;
                            }
                        }
                        break;
                    case RETURN:
                        if (gamepad_bits.start)
                            exit = true;
                        break;

                    case START_DEMO:
                        if (gamepad_bits.start) {
                            demo_requested = true;
                            restart = true;
                            exit = true;
                        }
                        break;

                    case ROM_SELECT:
                        if (gamepad_bits.start) {
                            demo_active = false;
                            demo_requested = false;
                            demo_advance_pending = false;
                            restart = true;
                            return;
                        }
                        break;
                    default:
                        break;
                }

                if (nullptr != item->callback && gamepad_bits.start) {
                    exit = item->callback();
                }
            }
            if(previous_palette!=manual_palette_selected) {
                hex_editing=false; game_palette_active=false;
                if(manual_palette_selected==PALETTE_CUSTOM_PRESET) { custom_rgb[0]=preset_rgb0[preset_rgb_index[0]]; custom_rgb[1]=preset_rgb1_at(preset_rgb_index[1]); custom_rgb[2]=preset_rgb2_at(preset_rgb_index[2]); custom_rgb[3]=preset_rgb3[preset_rgb_index[3]]; }
                if(manual_palette_selected==PALETTE_CUSTOM_RANDOM) randomize_custom_palette();
                apply_dmg_palette(); previous_palette=manual_palette_selected;
            }
            static char result[TEXTMODE_COLS];
            switch (item->type) {
                case HEX: {
                    int pidx=(int)((uint32_t*)item->value-custom_rgb);
                    if(manual_palette_selected==PALETTE_CUSTOM_PRESET) snprintf(result,TEXTMODE_COLS,"RGB%d: <%06lXh> %u",pidx,(unsigned long)custom_rgb[pidx],preset_rgb_index[pidx]+1);
                    else if(manual_palette_selected==PALETTE_CUSTOM_RANDOM) snprintf(result,TEXTMODE_COLS,"RGB%d: %06lXh RANDOM",pidx,(unsigned long)custom_rgb[pidx]);
                    else snprintf(result,TEXTMODE_COLS,item->text,(unsigned long)custom_rgb[pidx]);
                    break; }
                case GAME_PALETTE_LINK:
                    snprintf(result,TEXTMODE_COLS,"%s",!current_rom_name[0]?"Save for this game [N/A]":game_palette_linked?"Unlink game ini file":"Save for this game"); break;
                case INT:
                    snprintf(result, TEXTMODE_COLS, item->text, *(uint8_t *)item->value);
                    break;
                case ARRAY:
                    snprintf(result, TEXTMODE_COLS, item->text, item->value_list[*(uint8_t *)item->value]);
                    break;
                case TEXT:
                    snprintf(result, TEXTMODE_COLS, item->text, item->value);
                    break;
                case NONE:
                    color = 6;
                default:
                    snprintf(result, TEXTMODE_COLS, "%s", item->text);
            }
            draw_text(result, x, y, color, bg_color);
            if(item->type==ARRAY && item->value==&manual_palette_selected) for(uint8_t q=0;q<4;q++) draw_palette_preview(TEXTMODE_COLS-8+q*2,y,q,2);
            else if(item->type==HEX) draw_palette_preview(TEXTMODE_COLS-3,y,(uint8_t)((uint32_t*)item->value-custom_rgb),3);
        }

        if (gamepad_bits.down && !hex_editing) {
            current_item = (current_item + 1) % MENU_ITEMS_NUMBER;

            if (menu_items[current_item].type == NONE)
                current_item++;
        }
        if (gamepad_bits.up && !hex_editing) {
            current_item = (current_item - 1 + MENU_ITEMS_NUMBER) % MENU_ITEMS_NUMBER;

            if (menu_items[current_item].type == NONE)
                current_item--;
        }

        sleep_ms(125);
    }
    apply_dmg_palette();
    f_save_conf();
    graphics_set_mode(GRAPHICSMODE_DEFAULT);
}

int main() {
    overclock();

    ps2kbd.init_gpio();
    nespad_begin(clock_get_hz(clk_sys) / 1000, NES_GPIO_CLK, NES_GPIO_DATA, NES_GPIO_LAT);

    sem_init(&vga_start_semaphore, 0, 1);
    multicore_launch_core1(render_core);
    sem_release(&vga_start_semaphore);

    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    for (int i = 0; i < 6; i++) {
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, true);
        sleep_ms(33);
        gpio_put(PICO_DEFAULT_LED_PIN, false);
    }

    // Initialize I2S sound driver
    i2s_config_t i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
    i2s_config.dma_trans_count = AUDIO_SAMPLES;
    i2s_volume(&i2s_config, 0);
    i2s_init(&i2s_config);

    // Initialize audio emulation
    audio_init();

    FRESULT fr = f_mount(&fs, "", 1);
    if (FR_OK != fr) {
        printf("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
        /// TODO: error handling
        while(1);
    } else {
        f_mkdir(HOME_DIR);
        f_load_conf();
    }

    bool need_browser = true;
    bool rom_loaded = false;
    while (true) {
        if (need_browser && FR_OK == fr) {
            graphics_set_mode(TEXTMODE_DEFAULT);
            demo_active = false;
            demo_advance_pending = false;
            const bool rom_selected = filebrowser(HOME_DIR, "gbc,gb");

            if (demo_requested) {
                demo_requested = false;
                demo_active = true;
                demo_current_name[0] = 0;
                if (!demo_load_next_rom(nullptr)) {
                    demo_active = false;
                    continue;
                }
                rom_loaded = true;
            } else {
                rom_loaded = rom_selected;
            }
            graphics_set_mode(GRAPHICSMODE_DEFAULT);
            need_browser = false;
        }

        if (!rom_loaded) {
            need_browser = true;
            continue;
        }

        /* Initialise GB context. */
        gb_init_error_e ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
                                      &gb_cart_ram_write, &gb_error, nullptr);

        if (ret != GB_INIT_NO_ERROR) {
            graphics_set_mode(TEXTMODE_DEFAULT);
            draw_text("ERROR: invalid Game Boy ROM", 1, 1, 13, 1);
            sleep_ms(demo_active ? 1500 : 5000);
            if (demo_active) {
                if (demo_load_next_rom(demo_current_name)) {
                    rom_loaded = true;
                    continue;
                }
                demo_active = false;
            }
            rom_loaded = false;
            need_browser = true;
            continue;
        }

        /* Assign palette; per-game INI overrides it except in RANDOM mode. */
        if (manual_palette_selected == PALETTE_CUSTOM_RANDOM) { game_palette_linked = game_palette_read(); manual_palette_selected = PALETTE_CUSTOM_RANDOM; game_palette_active = false; randomize_custom_palette(); }
        else game_palette_read();
        if (!gb.cgb.cgbMode) apply_dmg_palette();

        gb_init_lcd(&gb, &lcd_draw_line);
        /* Load Save File. */
        read_cart_ram_file(&gb);

        //=============================================================================
        while (!restart) {
            //------------------------------------------------------------------------------
            if (fxPressedV) {
                if (altPressed) {
                    save_slot = fxPressedV;
                    load();
                } else if (ctrlPressed) {
                    save_slot = fxPressedV;
                    save();
                }
            }
            gb.direct.joypad_bits.up = !gamepad_bits.up;
            gb.direct.joypad_bits.down = !gamepad_bits.down;
            gb.direct.joypad_bits.left = !gamepad_bits.left;
            gb.direct.joypad_bits.right = !gamepad_bits.right;
            gb.direct.joypad_bits.a = !gamepad_bits.a;
            gb.direct.joypad_bits.b = !gamepad_bits.b;
            gb.direct.joypad_bits.select = !gamepad_bits.select;
            gb.direct.joypad_bits.start = !gamepad_bits.start;

            //gb.direct.joypad = nespad_state;
            //------------------------------------------------------------------------------
            /* hotkeys (select + * combo)*/
            if (!(gb.direct.joypad & 0b00001100) || nespad_state & DPAD_X) {

                static int keydown_counter = 0;
                char romname[24];

                gb_get_rom_name(&gb, romname);

                if (nullptr != strstr(romname, "ZELDA")) {
                    // half a second
                    if (keydown_counter++ > 30) {
                        menu();
                        keydown_counter = 0;
                    }
                }

                else {
                    menu();
                }
            }
            // TODO F2
            if ((nespad_state & DPAD_RT)) {
                // wait for release to prevent cycle
                while (nespad_state & DPAD_RT) {
                    sleep_ms(500);
                }
                load();
            }

            // TODO F3
            if ((nespad_state & DPAD_LT)) {
                // wait for release to prevent cycle
                while (nespad_state & DPAD_LT) {
                    sleep_ms(500);
                }
                save();
            }

            //-----------------------------------------------------------------
            gb_run_frame(&gb);

            if (demo_active) {
                const uint8_t di = demo_duration < count_of(demo_seconds) ? demo_duration : 0;
                if (time_us_64() - demo_game_started_at >= (uint64_t)demo_seconds[di] * 1000000ull) {
                    demo_advance_pending = true;
                    restart = true;
                }
            }

            //gb.direct.interlace = 1;

            if (!gb.direct.frame_skip) {
                audio_callback(NULL, reinterpret_cast<int16_t *>(stream), AUDIO_BUFFER_SIZE_BYTES);
                i2s_dma_write(&i2s_config, reinterpret_cast<const int16_t *>(stream));
            }
        }
        write_cart_ram_file(&gb);
        restart = false;
        rom_loaded = false;

        if (demo_requested) {
            demo_requested = false;
            demo_active = true;
            demo_current_name[0] = 0;
            if (demo_load_next_rom(nullptr)) {
                rom_loaded = true;
                continue;
            }
            demo_active = false;
        } else if (demo_active && demo_advance_pending) {
            demo_advance_pending = false;
            if (demo_load_next_rom(demo_current_name)) {
                rom_loaded = true;
                continue;
            }
            demo_active = false;
        }

        demo_active = false;
        demo_requested = false;
        demo_advance_pending = false;
        need_browser = true;
    }
}
