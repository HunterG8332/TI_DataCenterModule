#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

/* Minimal SSD1306 driver: 128x64, horizontal addressing mode, one
 * in-RAM framebuffer, and a hand-built 5x7 font covering only the
 * characters callers currently need (see s_font in ssd1306.c). */

#define SSD1306_WIDTH  128
#define SSD1306_HEIGHT 64
#define SSD1306_PAGES  (SSD1306_HEIGHT / 8)

typedef struct {
    i2c_master_dev_handle_t i2c_dev;
    uint8_t framebuffer[SSD1306_WIDTH * SSD1306_PAGES];
} ssd1306_t;

esp_err_t ssd1306_init(i2c_master_bus_handle_t bus, uint8_t i2c_addr, ssd1306_t *dev);

void ssd1306_clear(ssd1306_t *dev);

/* Draws text starting at pixel column `col` on 8-pixel-tall `page` (0..SSD1306_PAGES-1),
 * with each character's pixels replicated scale*scale (scale=1 is the native 6x8px cell).
 * Unsupported characters are skipped; a character that would run past the panel's right
 * edge stops the whole string rather than wrapping. */
void ssd1306_draw_text(ssd1306_t *dev, uint8_t col, uint8_t page, uint8_t scale, const char *text);

/* Pushes the framebuffer to the panel over I2C. */
esp_err_t ssd1306_flush(ssd1306_t *dev);
