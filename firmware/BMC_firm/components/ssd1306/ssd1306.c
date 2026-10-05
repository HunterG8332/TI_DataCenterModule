#include "ssd1306.h"

#include <string.h>

#define SSD1306_CTRL_CMD    0x00
#define SSD1306_CTRL_DATA   0x40

#define SSD1306_XFER_TIMEOUT_MS 1000

/* Command bytes per the SSD1306 datasheet's own initialization flow
 * (Solomon Systech app note), except DA=0x12 rather than the app note's
 * example DA=0x02: 0x02 selects sequential COM pin mapping, but real
 * 128x64 panels are wired for alternative COM pin mapping (0x12) --
 * see datasheet Section 10.1.18. Using 0x02 here shows a squashed,
 * interleaved image on actual 128x64 hardware. */
static const uint8_t s_init_cmds[] = {
    0xAE,             /* display off */
    0xD5, 0x80,       /* display clock divide ratio / osc freq */
    0xA8, 0x3F,       /* multiplex ratio = 63 (64 MUX) */
    0xD3, 0x00,       /* display offset = 0 */
    0x40,             /* display start line = 0 */
    0x8D, 0x14,       /* charge pump enable (internal Vcc) */
    0x20, 0x00,       /* memory addressing mode = horizontal */
    0xA1,             /* segment re-map: column 127 -> SEG0 */
    0xC8,             /* COM output scan direction: remapped (COM63 -> COM0) */
    0xDA, 0x12,       /* COM pins hw config: alternative, no L/R remap */
    0x81, 0x7F,       /* contrast */
    0xA4,             /* resume to RAM content display */
    0xA6,             /* normal (not inverted) display */
    0xAF,             /* display on */
};

static esp_err_t write_cmd_bytes(i2c_master_dev_handle_t dev, const uint8_t *cmds, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t pkt[2] = { SSD1306_CTRL_CMD, cmds[i] };
        esp_err_t err = i2c_master_transmit(dev, pkt, sizeof(pkt), SSD1306_XFER_TIMEOUT_MS);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, uint8_t i2c_addr, ssd1306_t *dev)
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = 400000,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev->i2c_dev);
    if (err != ESP_OK) {
        return err;
    }

    err = write_cmd_bytes(dev->i2c_dev, s_init_cmds, sizeof(s_init_cmds));
    if (err != ESP_OK) {
        return err;
    }

    ssd1306_clear(dev);
    return ssd1306_flush(dev);
}

void ssd1306_clear(ssd1306_t *dev)
{
    memset(dev->framebuffer, 0, sizeof(dev->framebuffer));
}

esp_err_t ssd1306_flush(ssd1306_t *dev)
{
    uint8_t addr_cmds[] = {
        0x21, 0, SSD1306_WIDTH - 1,   /* column address range */
        0x22, 0, SSD1306_PAGES - 1,   /* page address range */
    };
    esp_err_t err = write_cmd_bytes(dev->i2c_dev, addr_cmds, sizeof(addr_cmds));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t pkt[1 + sizeof(dev->framebuffer)];
    pkt[0] = SSD1306_CTRL_DATA;
    memcpy(&pkt[1], dev->framebuffer, sizeof(dev->framebuffer));
    return i2c_master_transmit(dev->i2c_dev, pkt, sizeof(pkt), SSD1306_XFER_TIMEOUT_MS);
}

/* 5x7 glyphs, one byte per column, bit0 = top pixel. Covers only what a
 * temperature readout and a fan duty / "DISABLED" status line need. */
typedef struct {
    char ch;
    uint8_t cols[5];
} glyph_t;

static const glyph_t s_font[] = {
    { ' ', { 0x00, 0x00, 0x00, 0x00, 0x00 } },
    { '-', { 0x08, 0x08, 0x08, 0x08, 0x08 } },
    { '.', { 0x00, 0x60, 0x60, 0x00, 0x00 } },
    { '%', { 0x23, 0x13, 0x08, 0x64, 0x62 } },
    { '0', { 0x3E, 0x51, 0x49, 0x45, 0x3E } },
    { '1', { 0x00, 0x42, 0x7F, 0x40, 0x00 } },
    { '2', { 0x42, 0x61, 0x51, 0x49, 0x46 } },
    { '3', { 0x21, 0x41, 0x45, 0x4B, 0x31 } },
    { '4', { 0x18, 0x14, 0x12, 0x7F, 0x10 } },
    { '5', { 0x27, 0x45, 0x45, 0x45, 0x39 } },
    { '6', { 0x3C, 0x4A, 0x49, 0x49, 0x30 } },
    { '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } },
    { '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } },
    { '9', { 0x06, 0x49, 0x49, 0x29, 0x1E } },
    { 'A', { 0x7E, 0x11, 0x11, 0x11, 0x7E } },
    { 'B', { 0x7F, 0x49, 0x49, 0x49, 0x36 } },
    { 'C', { 0x3E, 0x41, 0x41, 0x41, 0x22 } },
    { 'D', { 0x7F, 0x41, 0x41, 0x41, 0x3E } },
    { 'E', { 0x7F, 0x49, 0x49, 0x49, 0x41 } },
    { 'F', { 0x7F, 0x09, 0x09, 0x09, 0x01 } },
    { 'I', { 0x00, 0x41, 0x7F, 0x41, 0x00 } },
    { 'L', { 0x7F, 0x40, 0x40, 0x40, 0x40 } },
    { 'N', { 0x7F, 0x02, 0x04, 0x08, 0x7F } },
    { 'S', { 0x46, 0x49, 0x49, 0x49, 0x31 } },
    { 'W', { 0x3F, 0x40, 0x38, 0x40, 0x3F } },
};
#define FONT_GLYPH_WIDTH 5
#define FONT_ADVANCE     (FONT_GLYPH_WIDTH + 1)

static const uint8_t *find_glyph(char ch)
{
    for (size_t i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++) {
        if (s_font[i].ch == ch) {
            return s_font[i].cols;
        }
    }
    return NULL;
}

/* Pixel-replicates one 8-tall source column (bit i = pixel row i) into a
 * scale*8-tall, scale-wide block starting at (col, page_base). Used to
 * blow up the 5x7 font without needing a second, larger bitmap font. */
static void draw_scaled_column(ssd1306_t *dev, uint8_t col, uint8_t page_base,
                                uint8_t scale, uint8_t src_byte)
{
    for (uint8_t out_row = 0; out_row < 8 * scale; out_row++) {
        uint8_t src_bit = out_row / scale;
        if (!((src_byte >> src_bit) & 0x01)) {
            continue;
        }
        uint8_t page = page_base + out_row / 8;
        if (page >= SSD1306_PAGES) {
            continue;
        }
        uint8_t bit_in_page = out_row % 8;
        for (uint8_t dx = 0; dx < scale; dx++) {
            uint8_t out_col = col + dx;
            if (out_col >= SSD1306_WIDTH) {
                continue;
            }
            dev->framebuffer[page * SSD1306_WIDTH + out_col] |= (1 << bit_in_page);
        }
    }
}

void ssd1306_draw_text(ssd1306_t *dev, uint8_t col, uint8_t page, uint8_t scale, const char *text)
{
    if (scale == 0) {
        scale = 1;
    }

    for (const char *p = text; *p != '\0'; p++) {
        if (col + FONT_GLYPH_WIDTH * scale > SSD1306_WIDTH) {
            break;
        }
        const uint8_t *glyph = find_glyph(*p);
        if (glyph != NULL) {
            for (uint8_t i = 0; i < FONT_GLYPH_WIDTH; i++) {
                draw_scaled_column(dev, col + i * scale, page, scale, glyph[i]);
            }
        }
        col += FONT_ADVANCE * scale;
    }
}
