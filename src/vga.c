#include "vga.h"
#include "vbe.h"
#include "font8x8_basic.h"
#include <stdint.h>

#define FONT_PX 8
#define SCALE 2
#define CHAR_W (FONT_PX * SCALE)
#define CHAR_H (FONT_PX * SCALE)

static uint32_t screen_cols = 0;
static uint32_t screen_rows = 0;
static int terminal_row = 0;
static int terminal_column = 0;
static unsigned char terminal_color = (VGA_COLOR_BLACK << 4) | VGA_COLOR_LIGHT_GREEN;

// Стандартная 16-цветная VGA-палитра в RGB (0x00RRGGBB)
static const uint32_t vga_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

unsigned char vga_entry_color(enum vga_color fg, enum vga_color bg) {
    return fg | (bg << 4);
}

static void vbe_ensure_init(void) {
    if (screen_cols != 0) return;
    if (!vbe_init(1920, 1080, 32)) {
        screen_cols = 80;
        screen_rows = 25;
        return;
    }
    const vbe_info_t *info = vbe_get_info();
    screen_cols = info->width / CHAR_W;
    screen_rows = info->height / CHAR_H;
}

static void put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    const vbe_info_t *info = vbe_get_info();
    if (!info->fb || x >= info->width || y >= info->height) return;
    uint32_t *row = (uint32_t *)((uint8_t *)info->fb + y * info->pitch);
    row[x] = color;
}

static void draw_glyph(int col, int row, char c, uint32_t fg, uint32_t bg) {
    unsigned char uc = (unsigned char)c;
    const char *glyph = (uc < 128) ? font8x8_basic[uc] : font8x8_basic[(unsigned char)'?'];
    uint32_t base_x = (uint32_t)col * CHAR_W;
    uint32_t base_y = (uint32_t)row * CHAR_H;

    for (int fy = 0; fy < FONT_PX; fy++) {
        uint8_t bits = (uint8_t)glyph[fy];
        for (int fx = 0; fx < FONT_PX; fx++) {
            uint32_t color = (bits & (1 << fx)) ? fg : bg;
            for (int sy = 0; sy < SCALE; sy++) {
                for (int sx = 0; sx < SCALE; sx++) {
                    put_pixel(base_x + fx * SCALE + sx, base_y + fy * SCALE + sy, color);
                }
            }
        }
    }
}

void clear(void) {
    vbe_ensure_init();
    uint32_t bg = vga_palette[(terminal_color >> 4) & 0xF];
    const vbe_info_t *info = vbe_get_info();
    if (info->fb) {
        for (uint32_t y = 0; y < info->height; y++) {
            uint32_t *row = (uint32_t *)((uint8_t *)info->fb + y * info->pitch);
            for (uint32_t x = 0; x < info->width; x++) row[x] = bg;
        }
    }
    terminal_row = 0;
    terminal_column = 0;
}

void setcolor(unsigned char color) {
    terminal_color = color;
}

static void scroll(void) {
    if (terminal_row < (int)screen_rows) return;

    const vbe_info_t *info = vbe_get_info();
    if (info->fb) {
        uint8_t *fb8 = (uint8_t *)info->fb;
        uint32_t shift_bytes = CHAR_H * info->pitch;
        uint32_t total_bytes = info->height * info->pitch;

        for (uint32_t off = 0; off + shift_bytes < total_bytes; off++) {
            fb8[off] = fb8[off + shift_bytes];
        }

        uint32_t bg = vga_palette[(terminal_color >> 4) & 0xF];
        for (uint32_t y = info->height - CHAR_H; y < info->height; y++) {
            uint32_t *row = (uint32_t *)(fb8 + y * info->pitch);
            for (uint32_t x = 0; x < info->width; x++) row[x] = bg;
        }
    }
    terminal_row = (int)screen_rows - 1;
}

void putchar(char c) {
    vbe_ensure_init();
    uint32_t fg = vga_palette[terminal_color & 0xF];
    uint32_t bg = vga_palette[(terminal_color >> 4) & 0xF];

    if (c == '\n') {
        terminal_column = 0;
        terminal_row++;
    } else if (c == '\r') {
        terminal_column = 0;
    } else if (c == '\b') {
        if (terminal_column > 0) {
            terminal_column--;
            draw_glyph(terminal_column, terminal_row, ' ', fg, bg);
        }
    } else if (c == '\t') {
        terminal_column = (terminal_column + 4) & ~3;
    } else {
        draw_glyph(terminal_column, terminal_row, c, fg, bg);
        terminal_column++;
    }

    if (terminal_column >= (int)screen_cols) {
        terminal_column = 0;
        terminal_row++;
    }

    scroll();
}

void write(const char* data) {
    for (int i = 0; data[i] != '\0'; i++) {
        putchar(data[i]);
    }
}

static void print_dec(int num) {
    if (num == 0) {
        putchar('0');
        return;
    }
    if (num < 0) {
        putchar('-');
        num = -num;
    }
    char buf[32];
    int i = 0;
    while (num > 0) {
        buf[i++] = '0' + (num % 10);
        num /= 10;
    }
    while (i > 0) {
        putchar(buf[--i]);
    }
}

static void print_hex(unsigned int num) {
    write("0x");
    if (num == 0) {
        putchar('0');
        return;
    }
    char buf[32];
    char hex_digits[] = "0123456789ABCDEF";
    int i = 0;
    while (num > 0) {
        buf[i++] = hex_digits[num % 16];
        num /= 16;
    }
    while (i > 0) {
        putchar(buf[--i]);
    }
}

void print(const char* format, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, format);

    for (int i = 0; format[i] != '\0'; i++) {
        if (format[i] == '%') {
            i++;
            switch (format[i]) {
                case 's': {
                    const char* arg = __builtin_va_arg(args, const char*);
                    write(arg ? arg : "(null)");
                    break;
                }
                case 'c': {
                    char arg = (char)__builtin_va_arg(args, int);
                    putchar(arg);
                    break;
                }
                case 'd': {
                    int arg = __builtin_va_arg(args, int);
                    print_dec(arg);
                    break;
                }
                case 'x': {
                    unsigned int arg = __builtin_va_arg(args, unsigned int);
                    print_hex(arg);
                    break;
                }
                case '%': {
                    putchar('%');
                    break;
                }
                default: {
                    putchar('%');
                    putchar(format[i]);
                    break;
                }
            }
        } else {
            putchar(format[i]);
        }
    }

    __builtin_va_end(args);
}

void disable_cursor(void) {

}
