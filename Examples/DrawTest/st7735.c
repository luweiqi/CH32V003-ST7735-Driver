/*
 * ST7735 Driver for CH32V003
 *
 * Reference:
 *  - https://github.com/moononournation/Arduino_GFX
 *  - https://gitee.com/morita/ch32-v003/tree/master/Driver
 *  - https://github.com/cnlohr/ch32v003fun/tree/master/examples/spi_oled
 *
 * Aug 2023 by Li Mingjie
 *  - Email:  limingjie@outlook.com
 *  - GitHub: https://github.com/limingjie/
 */

#include "st7735.h"

#ifdef PLATFORMIO
    #include <debug.h>
#else
    #include "ch32v003fun.h"
#endif

#include "font5x7.h"

// Wiring
// | CH32V003       | ST7735    | Power | Description                       |
// | -------------- | --------- | ----- | --------------------------------- |
// |                | 1 - LEDA  | 3V3   | Use PWM to control brightness     |
// |                | 2 - GND   | GND   | GND                               |
// | PC2            | 3 - RESET |       | Reset                             |
// | PC3            | 4 - RS    |       | DC (Data / Command)               |
// | PC6 (SPI MOSI) | 5 - SDA   |       | SPI MOSI (Master Output Slave In) |
// | PC5 (SPI SCLK) | 6 - SCL   |       | SPI SCLK (Serial Clock)           |
// |                | 7 - VDD   | 3V3   | VDD                               |
// | PC4            | 8 - CS    |       | SPI CS/SS (Chip/Slave Select)     |

// Pin definitions
#define PIN_RESET 2  // PC2
#define PIN_DC    3  // PC3
#ifndef ST7735_NO_CS
    #define PIN_CS 4  // PC4
#endif
#define SPI_SCLK 5  // PC5
#define SPI_MOSI 6  // PC6

#define DATA_MODE()    (GPIOC->BSHR |= 1 << PIN_DC)  // DC High
#define COMMAND_MODE() (GPIOC->BCR |= 1 << PIN_DC)   // DC Low
#define RESET_HIGH()   (GPIOC->BSHR |= 1 << PIN_RESET)
#define RESET_LOW()    (GPIOC->BCR |= 1 << PIN_RESET)
#ifndef ST7735_NO_CS
    #define BEGIN_WRITE() (GPIOC->BCR |= 1 << PIN_CS)   // CS Low
    #define END_WRITE()   (GPIOC->BSHR |= 1 << PIN_CS)  // CS High
#else
    #define BEGIN_WRITE()
    #define END_WRITE()
#endif

#define ST7735_RST_DELAY    50   // delay ms wait for reset finish
#define ST7735_SLPOUT_DELAY 120  // delay ms wait for sleep out finish

// Commands
#define ST7735_SLPOUT  0x11
#define ST7735_INVOFF  0x20
#define ST7735_INVON   0x21
#define ST7735_NORON   0x13
#define ST7735_DISPON  0x29
#define ST7735_MADCTL  0x36
#define ST7735_COLMOD  0x3A
#define ST7735_GMCTRP1 0xE0
#define ST7735_GMCTRN1 0xE1
#define ST7735_CASET   0x2A
#define ST7735_RASET   0x2B
#define ST7735_RAMWR   0x2C
#define ST7735_RAMRD   0x2E

// Rotation & Color
#define ST7735_MADCTL_MH  0x04
#define ST7735_MADCTL_ML  0x10
#define ST7735_MADCTL_MV  0x20
#define ST7735_MADCTL_MX  0x40
#define ST7735_MADCTL_MY  0x80
#define ST7735_MADCTL_RGB 0x00
#define ST7735_MADCTL_BGR 0x08

#ifdef PLATFORMIO
    #define CTLR1_SPE_Set      ((uint16_t)0x0040)
    #define GPIO_CNF_OUT_PP    0x00
    #define GPIO_CNF_OUT_PP_AF 0x08
#endif

static uint16_t _cursor_x                 = 0;
static uint16_t _cursor_y                 = 0;      // Cursor position (x, y)
static uint16_t _color                    = BLACK;  // Color
static uint16_t _bg_color                 = WHITE;  // Background color
static uint8_t  buffer[ST7735_WIDTH << 1] = {0};    // DMA buffer, long enough to fill a row.

static void SPI_init(void)
{
    // Enable GPIO Port C and SPI peripheral
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOC | RCC_APB2Periph_SPI1;

    // PC2 - RESET
    GPIOC->CFGLR &= ~(0xf << (PIN_RESET << 2));
    GPIOC->CFGLR |= (GPIO_CNF_OUT_PP | GPIO_Speed_50MHz) << (PIN_RESET << 2);

    // PC3 - DC
    GPIOC->CFGLR &= ~(0xf << (PIN_DC << 2));
    GPIOC->CFGLR |= (GPIO_CNF_OUT_PP | GPIO_Speed_50MHz) << (PIN_DC << 2);

#ifndef ST7735_NO_CS
    // PC4 - CS
    GPIOC->CFGLR &= ~(0xf << (PIN_CS << 2));
    GPIOC->CFGLR |= (GPIO_CNF_OUT_PP | GPIO_Speed_50MHz) << (PIN_CS << 2);
#endif

    // PC5 - SCLK
    GPIOC->CFGLR &= ~(0xf << (SPI_SCLK << 2));
    GPIOC->CFGLR |= (GPIO_CNF_OUT_PP_AF | GPIO_Speed_50MHz) << (SPI_SCLK << 2);

    // PC6 - MOSI
    GPIOC->CFGLR &= ~(0xf << (SPI_MOSI << 2));
    GPIOC->CFGLR |= (GPIO_CNF_OUT_PP_AF | GPIO_Speed_50MHz) << (SPI_MOSI << 2);

    // Configure SPI
    SPI1->CTLR1 = SPI_CPHA_1Edge             // Bit 0     - Clock PHAse
                  | SPI_CPOL_Low             // Bit 1     - Clock POLarity - idles at the logical low voltage
                  | SPI_Mode_Master          // Bit 2     - Master device
                  | SPI_BaudRatePrescaler_2  // Bit 3-5   - F_HCLK / 2
                  | SPI_FirstBit_MSB         // Bit 7     - MSB transmitted first
                  | SPI_NSS_Soft             // Bit 9     - Software slave management
                  | SPI_DataSize_8b          // Bit 11    - 8-bit data
                  | SPI_Direction_1Line_Tx;  // Bit 14-15 - 1-line SPI, transmission only
    SPI1->CRCR = 7;                          // CRC
    SPI1->CTLR2 |= SPI_I2S_DMAReq_Tx;        // Configure SPI DMA Transfer
    SPI1->CTLR1 |= CTLR1_SPE_Set;            // Bit 6     - Enable SPI

    // Enable DMA peripheral
    RCC->AHBPCENR |= RCC_AHBPeriph_DMA1;

    // Config DMA for SPI TX
    DMA1_Channel3->CFGR = DMA_DIR_PeripheralDST          // Bit 4     - Read from memory
                          | DMA_Mode_Circular            // Bit 5     - Circulation mode
                          | DMA_PeripheralInc_Disable    // Bit 6     - Peripheral address no change
                          | DMA_MemoryInc_Enable         // Bit 7     - Increase memory address
                          | DMA_PeripheralDataSize_Byte  // Bit 8-9   - 8-bit data
                          | DMA_MemoryDataSize_Byte      // Bit 10-11 - 8-bit data
                          | DMA_Priority_VeryHigh        // Bit 12-13 - Very high priority
                          | DMA_M2M_Disable;             // Bit 14    - Disable memory to memory mode
    DMA1_Channel3->PADDR = (uint32_t)&SPI1->DATAR;
}

// Send data through SPI DMA
static void SPI_send_DMA(const uint8_t* buffer, uint16_t size, uint16_t repeat)
{
    DMA1_Channel3->MADDR = (uint32_t)buffer;
    DMA1_Channel3->CNTR  = size;
    DMA1_Channel3->CFGR |= DMA_CFGR1_EN;  // Turn on channel

    // Circulate the buffer
    while (repeat--)
    {
        // Clear flag, start sending?
        DMA1->INTFCR = DMA1_FLAG_TC3;

        // Waiting for channel 3 transmission complete
        while (!(DMA1->INTFR & DMA1_FLAG_TC3))
            ;
    }

    DMA1_Channel3->CFGR &= ~DMA_CFGR1_EN;  // Turn off channel
}

// Send data directly through SPI
static inline void SPI_send(uint8_t data)
{
    // Send byte
    SPI1->DATAR = data;

    // Waiting for transmission complete
    while (!(SPI1->STATR & SPI_STATR_TXE))
        ;
}

// Write command
static void write_command_8(uint8_t cmd)
{
    COMMAND_MODE();
    SPI_send(cmd);
}

// Write 8-bit data
static void write_data_8(uint8_t data)
{
    DATA_MODE();
    SPI_send(data);
}

// Write 16-bit data
static void write_data_16(uint16_t data)
{
    DATA_MODE();
    SPI_send(data >> 8);
    SPI_send(data);
}

void tft_init(void)
{
    SPI_init();

    // Reset display
    RESET_LOW();
    Delay_Ms(ST7735_RST_DELAY);
    RESET_HIGH();
    Delay_Ms(ST7735_RST_DELAY);

    BEGIN_WRITE();

    // Out of sleep mode, no args, w/delay
    write_command_8(ST7735_SLPOUT);
    Delay_Ms(ST7735_SLPOUT_DELAY);

    // Set rotation
    write_command_8(ST7735_MADCTL);
    write_data_8(ST7735_MADCTL_MY | ST7735_MADCTL_MV | ST7735_MADCTL_BGR);  // 0 - Horizontal
    // write_data_8(ST7735_MADCTL_BGR);                                        // 1 - Vertical
    // write_data_8(ST7735_MADCTL_MX | ST7735_MADCTL_MV | ST7735_MADCTL_BGR);  // 2 - Horizontal
    // write_data_8(ST7735_MADCTL_MX | ST7735_MADCTL_MY | ST7735_MADCTL_BGR);  // 3 - Vertical

    // Set color mode, 16-bit color
    write_command_8(ST7735_COLMOD);
    write_data_8(0x05);

    // Gamma Adjustments (pos. polarity), 16 args.
    // (Not entirely necessary, but provides accurate colors)
    uint8_t gamma_p[] = {0x09, 0x16, 0x09, 0x20, 0x21, 0x1B, 0x13, 0x19,
                         0x17, 0x15, 0x1E, 0x2B, 0x04, 0x05, 0x02, 0x0E};
    write_command_8(ST7735_GMCTRP1);
    DATA_MODE();
    SPI_send_DMA(gamma_p, 16, 1);

    // Gamma Adjustments (neg. polarity), 16 args.
    // (Not entirely necessary, but provides accurate colors)
    uint8_t gamma_n[] = {0x0B, 0x14, 0x08, 0x1E, 0x22, 0x1D, 0x18, 0x1E,
                         0x1B, 0x1A, 0x24, 0x2B, 0x06, 0x06, 0x02, 0x0F};
    write_command_8(ST7735_GMCTRN1);
    DATA_MODE();
    SPI_send_DMA(gamma_n, 16, 1);

    Delay_Ms(10);

    // Invert display
    write_command_8(ST7735_INVON);
    // write_command_8(ST7735_INVOFF);

    // Normal display on, no args, w/delay
    write_command_8(ST7735_NORON);
    Delay_Ms(10);

    // Main screen turn on, no args, w/delay
    write_command_8(ST7735_DISPON);
    Delay_Ms(10);

    END_WRITE();
}

void tft_set_cursor(uint16_t x, uint16_t y)
{
    _cursor_x = x + ST7735_X_OFFSET;
    _cursor_y = y + ST7735_Y_OFFSET;
}

void tft_set_color(uint16_t color)
{
    _color = color;
}

void tft_set_background_color(uint16_t color)
{
    _bg_color = color;
}

static void tft_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    write_command_8(ST7735_CASET);
    write_data_16(x0);
    write_data_16(x1);
    write_command_8(ST7735_RASET);
    write_data_16(y0);
    write_data_16(y1);
    write_command_8(ST7735_RAMWR);
}

void tft_print_char(char c)
{
    const unsigned char* start = &font[c + (c << 2)];

    uint16_t sz = 0;
    for (uint8_t i = 0; i < 7; i++)
    {
        for (uint8_t j = 0; j < 5; j++)
        {
            if ((*(start + j)) & (0x01 << i))
            {
                buffer[sz++] = _color >> 8;
                buffer[sz++] = _color;
            }
            else
            {
                buffer[sz++] = _bg_color >> 8;
                buffer[sz++] = _bg_color;
            }
        }
    }

    BEGIN_WRITE();
    tft_set_window(_cursor_x, _cursor_y, _cursor_x + 4, _cursor_x + 6);
    DATA_MODE();
    SPI_send_DMA(buffer, sz, 1);
    END_WRITE();
}

void tft_print(const char* str)
{
    while (*str)
    {
        tft_print_char(*str++);
        _cursor_x += 6;
    }
}

void tft_print_number(int32_t num)
{
    static char str[12];
    uint8_t     digits = 11;
    uint8_t     neg    = 0;

    // Handle negative number
    if (num < 0)
    {
        neg = 1;
        num = -num;
    }

    str[digits] = '\0';  // End of the string.
    while (num)
    {
        str[--digits] = num % 10 + '0';
        num /= 10;
    }

    if (digits == 11)
    {
        str[--digits] = '0';
    }

    if (neg)
    {
        str[--digits] = '-';
    }

    // Fill in spaces
    while (digits)
    {
        str[--digits] = ' ';
    }

    tft_print(str);
}

void tft_draw_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    x += ST7735_X_OFFSET;
    y += ST7735_Y_OFFSET;
    BEGIN_WRITE();
    tft_set_window(x, y, x, y);
    write_data_16(color);
    END_WRITE();
}

void tft_fill_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color)
{
    x += ST7735_X_OFFSET;
    y += ST7735_Y_OFFSET;

    uint16_t sz = 0;
    for (uint16_t x = 0; x < width; x++)
    {
        buffer[sz++] = color >> 8;
        buffer[sz++] = color;
    }

    BEGIN_WRITE();
    tft_set_window(x, y, x + width - 1, y + height - 1);
    DATA_MODE();
    SPI_send_DMA(buffer, sz, height);
    END_WRITE();
}

void tft_draw_bitmap(uint16_t x, uint16_t y, uint16_t width, uint16_t height, const uint8_t* bitmap)
{
    x += ST7735_X_OFFSET;
    y += ST7735_Y_OFFSET;
    BEGIN_WRITE();
    tft_set_window(x, y, x + width - 1, y + height - 1);
    DATA_MODE();
    SPI_send_DMA(bitmap, width * height << 1, 1);
    END_WRITE();
}

// The draw line fuctions from Arduino GFX
// - https://github.com/moononournation/Arduino_GFX/blob/master/src/Arduino_GFX.cpp
// Some functions are DMA accelerated.
static void _tft_draw_fast_v_line(int16_t x, int16_t y, int16_t h, uint16_t color)
{
    x += ST7735_X_OFFSET;
    y += ST7735_Y_OFFSET;

    uint16_t sz = 0;
    for (int16_t j = 0; j < h; j++)
    {
        buffer[sz++] = color >> 8;
        buffer[sz++] = color;
    }

    BEGIN_WRITE();
    tft_set_window(x, y, x, y + h - 1);
    DATA_MODE();
    SPI_send_DMA(buffer, sz, 1);
    END_WRITE();
}

static void _tft_draw_fast_h_line(int16_t x, int16_t y, int16_t w, uint16_t color)
{
    x += ST7735_X_OFFSET;
    y += ST7735_Y_OFFSET;

    uint16_t sz = 0;
    for (int16_t j = 0; j < w; j++)
    {
        buffer[sz++] = color >> 8;
        buffer[sz++] = color;
    }

    BEGIN_WRITE();
    tft_set_window(x, y, x + w - 1, y);
    DATA_MODE();
    SPI_send_DMA(buffer, sz, 1);
    END_WRITE();
}

// Draw line helpers
#define _diff(a, b) ((a > b) ? (a - b) : (b - a))
#define _swap_int16_t(a, b) \
    {                       \
        int16_t t = a;      \
        a         = b;      \
        b         = t;      \
    }

static void _tft_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    uint8_t steep = _diff(y1, y0) > _diff(x1, x0);
    if (steep)
    {
        _swap_int16_t(x0, y0);
        _swap_int16_t(x1, y1);
    }

    if (x0 > x1)
    {
        _swap_int16_t(x0, x1);
        _swap_int16_t(y0, y1);
    }

    int16_t dx   = x1 - x0;
    int16_t dy   = _diff(y1, y0);
    int16_t err  = dx >> 1;
    int16_t step = (y0 < y1) ? 1 : -1;

    for (; x0 <= x1; x0++)
    {
        if (steep)
        {
            tft_draw_pixel(y0, x0, color);
        }
        else
        {
            tft_draw_pixel(x0, y0, color);
        }
        err -= dy;
        if (err < 0)
        {
            err += dx;
            y0 += step;
        }
    }
}

void tft_draw_rect(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint16_t color)
{
    _tft_draw_fast_h_line(x, y, width, color);
    _tft_draw_fast_h_line(x, y + height - 1, width, color);
    _tft_draw_fast_v_line(x, y, height, color);
    _tft_draw_fast_v_line(x + width - 1, y, height, color);
}

void tft_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    if (x0 == x1)
    {
        if (y0 > y1)
        {
            _swap_int16_t(y0, y1);
        }
        _tft_draw_fast_v_line(x0, y0, y1 - y0 + 1, color);
    }
    else if (y0 == y1)
    {
        if (x0 > x1)
        {
            _swap_int16_t(x0, x1);
        }
        _tft_draw_fast_h_line(x0, y0, x1 - x0 + 1, color);
    }
    else
    {
        _tft_draw_line(x0, y0, x1, y1, color);
    }
}
