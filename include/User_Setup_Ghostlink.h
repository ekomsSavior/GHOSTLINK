#pragma once

// TFT_eSPI setup for original ILI9341 and dual-USB ST7789 2.8-inch CYDs.
#define USER_SETUP_LOADED
#if defined(GHOST_DISPLAY_ST7789)
#define ST7789_DRIVER
#define TFT_RGB_ORDER TFT_BGR
#define TFT_INVERSION_OFF
#define USER_SETUP_INFO "GHOSTLINK CYD ST7789"
#else
#define ILI9341_DRIVER
#define USER_SETUP_INFO "GHOSTLINK CYD ILI9341"
#endif
#define USE_HSPI_PORT // Touch uses VSPI with its own pin mapping.

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  -1

#define TFT_BL 21
#define TFT_BACKLIGHT_ON HIGH

#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4

#define SPI_FREQUENCY      40000000
#define SPI_READ_FREQUENCY 20000000
