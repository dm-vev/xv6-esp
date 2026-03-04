/**
 * @file picasso_tdeck.c
 * @brief Picasso T-Deck Backend Implementation
 *
 * Hardware-specific backend for LilyGo T-Deck device.
 * Supports ST7789 320x240 LCD via SPI interface.
 *
 * Hardware configuration (T-Deck):
 * - Display: ST7789 320x240 16-bit color
 * - SPI Host: SPI2
 * - Pins:
 *   - MOSI: GPIO 41
 *   - MISO: GPIO 38
 *   - CLK:  GPIO 40
 *   - CS:   GPIO 12
 *   - DC:   GPIO 11
 *   - RST:  Not connected (GPIO_NUM_NC)
 *   - BL:   GPIO 42 (backlight)
 *   - PWR:  GPIO 10 (display power)
 */

#include "picasso.h"
#include "picasso_internal.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "xv6_module.h"
#include "core/spinlock.h"
#include "core/types.h"
#include "core/param.h"

/* Forward declarations for xv6 spinlock functions */
void initlock(struct spinlock *lk, char *name);
void acquire(struct spinlock *lk);
void release(struct spinlock *lk);

/* Forward declarations for ESP-IDF functions */
extern void ets_delay_us(uint32_t us);
extern void vTaskDelay(uint32_t ticks);
#define pdMS_TO_TICKS(ms) ((ms) / portTICK_PERIOD_MS)
extern uint32_t portTICK_PERIOD_MS;

/* ESP-IDF headers */
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_st7789.h>
#include <esp_lcd_panel_vendor.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <hal/spi_hal.h>
#include <esp_err.h>

/* ============================================================================
 * Hardware Configuration
 * ============================================================================ */

/* SPI configuration */
#define TDECK_SPI_HOST          SPI2_HOST
#define TDECK_SPI_MOSI_GPIO     GPIO_NUM_41
#define TDECK_SPI_MISO_GPIO     GPIO_NUM_38
#define TDECK_SPI_CLK_GPIO      GPIO_NUM_40

/* Display control pins */
#define TDECK_LCD_CS_GPIO       GPIO_NUM_12
#define TDECK_LCD_DC_GPIO       GPIO_NUM_11
#define TDECK_LCD_RST_GPIO      GPIO_NUM_NC  /* Not connected on T-Deck */
#define TDECK_LCD_BL_GPIO       GPIO_NUM_42  /* Backlight */
#define TDECK_LCD_PWR_GPIO      GPIO_NUM_10  /* Display power enable */

/* Display timing - 80 MHz for maximum performance */
#define TDECK_SPI_FREQ_HZ       (80 * 1000 * 1000)  /* 80 MHz - ST7789 max rated freq */
#define TDECK_TRANS_QUEUE_DEPTH   4                   /* Multiple transactions for DMA pipelining */

/* DMA configuration */
#define TDECK_DMA_BUFFER_SIZE     (320 * 2 * 40)      /* 40 lines buffer */
#define TDECK_MAX_TRANSFER_SIZE   (TDECK_DMA_BUFFER_SIZE * 2)  /* Double buffering for DMA */

/* Backlight configuration */
#define TDECK_BACKLIGHT_STEPS     16

/* ============================================================================
 * Internal State
 * ============================================================================ */

typedef struct {
    /* ESP-IDF handles */
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
    spi_device_handle_t spi_handle;
    spi_bus_config_t bus_config;

    /* State flags */
    uint8_t initialized;
    uint8_t power_on;
    uint8_t backlight_level;

    /* Backlight tracking for software PWM */
    uint8_t backlight_current;

    /* DMA buffers for zero-copy transfers */
    uint16_t *dma_buffer[2];       /* Ping-pong DMA buffers */
    uint8_t active_buffer;          /* Current active buffer */
    uint32_t dma_buffer_size;       /* Size of each DMA buffer in bytes */
    uint32_t pending_transactions;  /* Number of pending DMA transactions */

    /* Performance tracking */
    uint32_t frame_count;
    uint32_t dma_underrun_count;
    uint64_t last_frame_time_us;

    /* PSRAM memory tracking */
    uint32_t psram_size;
    uint32_t psram_free;
    uint32_t internal_free;

    /* Thread safety */
    struct spinlock lock;
} tdeck_state_t;

static tdeck_state_t g_tdeck;

/* ============================================================================
 * Hardware Initialization
 * ============================================================================ */

/**
 * @brief Initialize GPIO pins for display control
 */
static int tdeck_gpio_init(void)
{
    esp_err_t err;

    /* Configure backlight and power pins as output */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << TDECK_LCD_BL_GPIO) | (1ULL << TDECK_LCD_PWR_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    err = gpio_config(&io_conf);
    if (err != ESP_OK) {
        return -1;
    }

    /* Power on the display */
    gpio_set_level(TDECK_LCD_PWR_GPIO, 1);
    g_tdeck.power_on = 1;

    /* Start with backlight off */
    gpio_set_level(TDECK_LCD_BL_GPIO, 0);
    g_tdeck.backlight_level = 0;
    g_tdeck.backlight_current = 0;

    return 0;
}

/**
 * @brief Deinitialize GPIO
 */
static void tdeck_gpio_deinit(void)
{
    /* Turn off backlight and power */
    gpio_set_level(TDECK_LCD_BL_GPIO, 0);
    gpio_set_level(TDECK_LCD_PWR_GPIO, 0);
    g_tdeck.power_on = 0;
    g_tdeck.backlight_level = 0;
    g_tdeck.backlight_current = 0;
}

/**
 * @brief Initialize SPI bus for display
 */
static int tdeck_spi_init(void)
{
    esp_err_t err;

    /* Configure SPI bus with 80 MHz and DMA support */
    spi_bus_config_t buscfg = {
        .mosi_io_num = TDECK_SPI_MOSI_GPIO,
        .miso_io_num = TDECK_SPI_MISO_GPIO,
        .sclk_io_num = TDECK_SPI_CLK_GPIO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = TDECK_MAX_TRANSFER_SIZE,  /* Increased for DMA efficiency */
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
        .intr_flags = 0,  /* Use default interrupt level */
    };

    err = spi_bus_initialize(TDECK_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        /* ESP_ERR_INVALID_STATE means bus already initialized */
        return -1;
    }

    memcpy(&g_tdeck.bus_config, &buscfg, sizeof(buscfg));
    return 0;
}

/**
 * @brief Deinitialize SPI bus
 */
static void tdeck_spi_deinit(void)
{
    spi_bus_free(TDECK_SPI_HOST);
}

/**
 * @brief Initialize DMA buffers for zero-copy transfers
 *
 * Allocates ping-pong DMA buffers for maximum performance.
 * These buffers are used for partial screen updates.
 */
static int tdeck_dma_init(void)
{
    g_tdeck.dma_buffer_size = TDECK_DMA_BUFFER_SIZE;

    for (int i = 0; i < 2; i++) {
        g_tdeck.dma_buffer[i] = (uint16_t *)heap_caps_malloc(
            g_tdeck.dma_buffer_size,
            MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL
        );
        if (!g_tdeck.dma_buffer[i]) {
            /* Free previously allocated buffer */
            if (i > 0) {
                heap_caps_free(g_tdeck.dma_buffer[0]);
                g_tdeck.dma_buffer[0] = NULL;
            }
            return -1;
        }
        memset(g_tdeck.dma_buffer[i], 0, g_tdeck.dma_buffer_size);
    }

    g_tdeck.active_buffer = 0;
    g_tdeck.pending_transactions = 0;
    g_tdeck.frame_count = 0;
    g_tdeck.dma_underrun_count = 0;

    return 0;
}

/**
 * @brief Deinitialize DMA buffers
 */
static void tdeck_dma_deinit(void)
{
    for (int i = 0; i < 2; i++) {
        if (g_tdeck.dma_buffer[i]) {
            heap_caps_free(g_tdeck.dma_buffer[i]);
            g_tdeck.dma_buffer[i] = NULL;
        }
    }
    g_tdeck.pending_transactions = 0;
}

/**
 * @brief Get current DMA buffer for writing
 * @return Pointer to active DMA buffer
 */
static inline uint16_t* tdeck_dma_get_buffer(void)
{
    return g_tdeck.dma_buffer[g_tdeck.active_buffer];
}

/**
 * @brief Switch to next DMA buffer (ping-pong)
 */
static inline void tdeck_dma_swap_buffer(void)
{
    g_tdeck.active_buffer = 1 - g_tdeck.active_buffer;
}

/**
 * @brief Initialize LCD panel via ESP-IDF LCD driver
 */
static int tdeck_panel_init(void)
{
    esp_err_t err;

    /* Configure LCD IO via SPI with 80 MHz and optimized DMA settings */
    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = TDECK_LCD_CS_GPIO,
        .dc_gpio_num = TDECK_LCD_DC_GPIO,
        .pclk_hz = TDECK_SPI_FREQ_HZ,          /* 80 MHz for maximum throughput */
        .spi_mode = 0,                         /* Mode 0: CPOL=0, CPHA=0 - works best at 80MHz */
        .trans_queue_depth = TDECK_TRANS_QUEUE_DEPTH,  /* 4 transactions for DMA pipelining */
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = {
            /* No CS active high - standard behavior */
            .cs_active_high = 0,
            /* Allow DC line to use GPIO control for speed */
            .octal_mode = 0,
        },
    };

    err = esp_lcd_new_panel_io_spi(TDECK_SPI_HOST, &io_config, &g_tdeck.io_handle);
    if (err != ESP_OK) {
        return -1;
    }

    /* Create ST7789 panel */
    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = TDECK_LCD_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,  /* ST7789 uses BGR */
        .bits_per_pixel = PICASSO_BITS_PER_PIXEL,
        .flags = {
            .reset_active_high = 0,
        },
    };

    err = esp_lcd_new_panel_st7789(g_tdeck.io_handle, &panel_config, &g_tdeck.panel_handle);
    if (err != ESP_OK) {
        esp_lcd_panel_io_del(g_tdeck.io_handle);
        g_tdeck.io_handle = NULL;
        return -1;
    }

    /* Reset and initialize panel */
    err = esp_lcd_panel_reset(g_tdeck.panel_handle);
    if (err != ESP_OK) {
        esp_lcd_panel_del(g_tdeck.panel_handle);
        esp_lcd_panel_io_del(g_tdeck.io_handle);
        g_tdeck.panel_handle = NULL;
        g_tdeck.io_handle = NULL;
        return -1;
    }

    /* Delay after reset (spec requires >10ms) */
    vTaskDelay(pdMS_TO_TICKS(20));

    err = esp_lcd_panel_init(g_tdeck.panel_handle);
    if (err != ESP_OK) {
    esp_lcd_panel_del(g_tdeck.panel_handle);
    esp_lcd_panel_io_del(g_tdeck.io_handle);
        g_tdeck.panel_handle = NULL;
        g_tdeck.io_handle = NULL;
        return -1;
    }

    /* T-Deck display orientation: landscape, swapped axes */
    /* ST7789 MADCTL register: 0x36
     * Bit 7: MY (Row Address Order)
     * Bit 6: MX (Column Address Order)
     * Bit 5: MV (Row/Column Exchange)
     * Bit 4: ML (Vertical Refresh Order)
     * Bit 3: RGB/BGR (Color filter panel)
     * Bit 2: MH (Horizontal Refresh Order)
     */

    /* Set orientation to landscape with swapped XY */
    err = esp_lcd_panel_swap_xy(g_tdeck.panel_handle, true);
    if (err != ESP_OK) {
        /* Continue even if this fails */
    }

    /* No mirroring needed for T-Deck */
    err = esp_lcd_panel_mirror(g_tdeck.panel_handle, false, false);
    if (err != ESP_OK) {
        /* Continue even if this fails */
    }

    /* Send initialization commands */
    /* These are standard ST7789 init commands for T-Deck */
    const struct {
        uint8_t cmd;
        uint8_t data;
        uint8_t len;
    } init_cmds[] = {
        /* MADCTL: Memory Data Access Control
         * 0x68 = 0b01101000
         * Bit 6 (MX) = 1: Column address order reversed
         * Bit 5 (MV) = 1: Row/column exchange (swap XY)
         * Bit 3 (BGR) = 1: BGR color filter panel
         */
        {0x36, 0x68, 1},

        /* COLMOD: Interface Pixel Format
         * 0x55 = 16 bits per pixel
         */
        {0x3A, 0x55, 1},

        /* INVON: Display Inversion On
         * Required for some ST7789 panels
         */
        {0x21, 0x00, 0},

        /* SLPOUT: Sleep Out */
        {0x11, 0x00, 0},

        /* DISPON: Display On */
        {0x29, 0x00, 0},
    };

    for (size_t i = 0; i < sizeof(init_cmds) / sizeof(init_cmds[0]); i++) {
        err = esp_lcd_panel_io_tx_param(
            g_tdeck.io_handle,
            init_cmds[i].cmd,
            init_cmds[i].len > 0 ? &init_cmds[i].data : NULL,
            init_cmds[i].len
        );

        if (err != ESP_OK) {
            /* Log but continue */
        }

        /* Delay after SLPOUT command (spec requires 120ms) */
        if (init_cmds[i].cmd == 0x11) {
            vTaskDelay(pdMS_TO_TICKS(120));
        }
    }

    return 0;
}

/**
 * @brief Deinitialize LCD panel
 */
static void tdeck_panel_deinit(void)
{
    if (g_tdeck.panel_handle) {
        /* Turn off display */
        esp_lcd_panel_io_tx_param(g_tdeck.io_handle, 0x28, NULL, 0);  /* DISPOFF */
        esp_lcd_panel_io_tx_param(g_tdeck.io_handle, 0x10, NULL, 0);  /* SLPIN */

        esp_lcd_panel_del(g_tdeck.panel_handle);
        g_tdeck.panel_handle = NULL;
    }

    if (g_tdeck.io_handle) {
        esp_lcd_panel_io_del(g_tdeck.io_handle);
        g_tdeck.io_handle = NULL;
    }
}

/* ============================================================================
 * Backlight Control
 * ============================================================================ */

/**
 * @brief Set backlight level using software PWM
 *
 * T-Deck uses a simple on/off backlight circuit controlled by GPIO.
 * We implement 16-step brightness control using pulse counting.
 */
static int tdeck_set_backlight(uint8_t level)
{
    if (!g_tdeck.initialized) {
        errno = ENODEV;
        return -1;
    }

    acquire(&g_tdeck.lock);

    /* Clamp to valid range */
    if (level > 255) {
        level = 255;
    }

    /* Convert 0-255 to 0-15 steps */
    uint8_t target_step = (level * (TDECK_BACKLIGHT_STEPS - 1) + 127) / 255;

    if (target_step == 0) {
        /* Turn off backlight */
        gpio_set_level(TDECK_LCD_BL_GPIO, 0);
        vTaskDelay(pdMS_TO_TICKS(3));  /* Required delay */
        g_tdeck.backlight_current = 0;
    } else if (g_tdeck.backlight_current == 0) {
        /* Turning on from off state */
        gpio_set_level(TDECK_LCD_BL_GPIO, 1);
        ets_delay_us(30);  /* Small delay */
        g_tdeck.backlight_current = TDECK_BACKLIGHT_STEPS;
    }

    /* Calculate pulse count for brightness adjustment */
    int from = TDECK_BACKLIGHT_STEPS - g_tdeck.backlight_current;
    int to = TDECK_BACKLIGHT_STEPS - target_step;
    int pulses = (TDECK_BACKLIGHT_STEPS + to - from) % TDECK_BACKLIGHT_STEPS;

    /* Send pulses to adjust brightness */
    for (int i = 0; i < pulses; i++) {
        gpio_set_level(TDECK_LCD_BL_GPIO, 0);
        ets_delay_us(1);
        gpio_set_level(TDECK_LCD_BL_GPIO, 1);
        ets_delay_us(1);
    }

    g_tdeck.backlight_current = target_step;
    g_tdeck.backlight_level = level;

    release(&g_tdeck.lock);
    return 0;
}

/* ============================================================================
 * Drawing Functions
 * ============================================================================ */

/**
 * @brief Draw bitmap to display
 *
 * Uses ESP-IDF LCD driver to transfer data via SPI DMA.
 */
static int tdeck_draw_bitmap(int16_t x1, int16_t y1, int16_t x2, int16_t y2, const void *data)
{
    esp_err_t err;

    if (!g_tdeck.initialized || !g_tdeck.panel_handle) {
        errno = ENODEV;
        return -1;
    }

    if (!data) {
        errno = EINVAL;
        return -1;
    }

    acquire(&g_tdeck.lock);

    /* Clamp coordinates */
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > PICASSO_DISPLAY_WIDTH) x2 = PICASSO_DISPLAY_WIDTH;
    if (y2 > PICASSO_DISPLAY_HEIGHT) y2 = PICASSO_DISPLAY_HEIGHT;

    if (x1 >= x2 || y1 >= y2) {
        release(&g_tdeck.lock);
        return 0;
    }

    /* For small bitmaps, use direct transfer */
    uint16_t width = x2 - x1;
    uint16_t height = y2 - y1;
    uint32_t pixel_count = width * height;
    uint32_t transfer_size = pixel_count * sizeof(uint16_t);

    /* Use DMA chunked transfer for large bitmaps */
    if (transfer_size > TDECK_DMA_BUFFER_SIZE && g_tdeck.dma_buffer[0]) {
        /* Use DMA buffer for chunked transfer */
        uint16_t lines_per_chunk = TDECK_DMA_BUFFER_SIZE / (width * sizeof(uint16_t));
        if (lines_per_chunk == 0) lines_per_chunk = 1;

        const uint16_t *src = (const uint16_t *)data;
        int current_y = y1;

        while (current_y < y2) {
            int lines_to_transfer = lines_per_chunk;
            if (current_y + lines_to_transfer > y2) {
                lines_to_transfer = y2 - current_y;
            }

            /* Select active DMA buffer (ping-pong) */
            uint16_t *dma_buf = tdeck_dma_get_buffer();
            uint32_t bytes_to_copy = lines_to_transfer * width * sizeof(uint16_t);
            memcpy(dma_buf, src, bytes_to_copy);

            /* Start DMA transfer for this chunk */
            err = esp_lcd_panel_draw_bitmap(
                g_tdeck.panel_handle,
                x1, current_y, x2, current_y + lines_to_transfer,
                dma_buf
            );

            if (err != ESP_OK) {
                g_tdeck.dma_underrun_count++;
                release(&g_tdeck.lock);
                return -1;
            }

            /* Switch to next buffer for next chunk */
            tdeck_dma_swap_buffer();
            g_tdeck.pending_transactions++;

            src += lines_to_transfer * width;
            current_y += lines_to_transfer;
        }

        g_tdeck.frame_count++;
    } else {
        /* Direct transfer for small bitmaps */
        err = esp_lcd_panel_draw_bitmap(
            g_tdeck.panel_handle,
            x1, y1, x2, y2,
            data
        );

        if (err != ESP_OK) {
            release(&g_tdeck.lock);
            return -1;
        }

        g_tdeck.frame_count++;
    }

    release(&g_tdeck.lock);

    return 0;
}

/**
 * @brief Wait for all pending SPI transactions to complete
 */
static void tdeck_wait_done(void)
{
    if (!g_tdeck.initialized || !g_tdeck.io_handle) {
        return;
    }

    acquire(&g_tdeck.lock);

    /* Issue dummy command to flush transaction queue */
    /* Command -1 with no data blocks until all pending transfers complete */
    esp_lcd_panel_io_tx_param(g_tdeck.io_handle, -1, NULL, 0);

    release(&g_tdeck.lock);
}

/**
 * @brief Set display power state
 */
static int tdeck_set_power(bool on)
{
    if (!g_tdeck.initialized) {
        errno = ENODEV;
        return -1;
    }

    acquire(&g_tdeck.lock);

    if (on) {
        gpio_set_level(TDECK_LCD_PWR_GPIO, 1);
        g_tdeck.power_on = 1;

        /* Wake up display if needed */
        if (g_tdeck.panel_handle) {
            esp_lcd_panel_io_tx_param(g_tdeck.io_handle, 0x11, NULL, 0);  /* SLPOUT */
            vTaskDelay(pdMS_TO_TICKS(120));
            esp_lcd_panel_io_tx_param(g_tdeck.io_handle, 0x29, NULL, 0);  /* DISPON */
        }
    } else {
        /* Turn off display */
        if (g_tdeck.panel_handle) {
            esp_lcd_panel_io_tx_param(g_tdeck.io_handle, 0x28, NULL, 0);  /* DISPOFF */
            esp_lcd_panel_io_tx_param(g_tdeck.io_handle, 0x10, NULL, 0);  /* SLPIN */
        }

        gpio_set_level(TDECK_LCD_PWR_GPIO, 0);
        g_tdeck.power_on = 0;
    }

    release(&g_tdeck.lock);
    return 0;
}

/* ============================================================================
 * Backend Interface
 * ============================================================================ */

/**
 * @brief Initialize T-Deck backend
 */
static int tdeck_backend_init(void)
{
    int ret;

    if (g_tdeck.initialized) {
        return 0;
    }

    memset(&g_tdeck, 0, sizeof(g_tdeck));
    initlock(&g_tdeck.lock, "tdeck");

    /* Initialize hardware in order: GPIO -> SPI -> Panel */
    ret = tdeck_gpio_init();
    if (ret < 0) {
        return ret;
    }

    ret = tdeck_spi_init();
    if (ret < 0) {
        tdeck_gpio_deinit();
        return ret;
    }

    ret = tdeck_panel_init();
    if (ret < 0) {
        tdeck_spi_deinit();
        tdeck_gpio_deinit();
        return ret;
    }

    /* Initialize DMA buffers after panel is ready */
    ret = tdeck_dma_init();
    if (ret < 0) {
        tdeck_panel_deinit();
        tdeck_spi_deinit();
        tdeck_gpio_deinit();
        return ret;
    }

    g_tdeck.initialized = 1;

    /* Record PSRAM statistics */
    g_tdeck.psram_size = psram_get_size();
    g_tdeck.psram_free = psram_get_free_size();
    g_tdeck.internal_free = internal_ram_get_free_size();

    /* Set initial backlight */
    tdeck_set_backlight(128); /* 50% brightness */

    return 0;
}

    g_tdeck.initialized = 1;

    /* Set initial backlight */
    tdeck_set_backlight(128);  /* 50% brightness */

    return 0;
}

/**
 * @brief Deinitialize T-Deck backend
 */
static void tdeck_backend_deinit(void)
{
    if (!g_tdeck.initialized) {
        return;
    }

    acquire(&g_tdeck.lock);

    /* Turn off backlight */
    gpio_set_level(TDECK_LCD_BL_GPIO, 0);

    /* Deinitialize in reverse order */
    tdeck_dma_deinit();      /* Free DMA buffers first */
    tdeck_panel_deinit();
    tdeck_spi_deinit();
    tdeck_gpio_deinit();

    g_tdeck.initialized = 0;
    release(&g_tdeck.lock);
}

/* Backend structure */
static const picasso_backend_t tdeck_backend = {
    .name = "tdeck",
    .init = tdeck_backend_init,
    .deinit = tdeck_backend_deinit,
    .set_backlight = tdeck_set_backlight,
    .draw_bitmap = tdeck_draw_bitmap,
    .wait_done = tdeck_wait_done,
    .set_power = tdeck_set_power,
};

/* ============================================================================
 * Public API
 * ============================================================================ */

const picasso_backend_t* picasso_tdeck_get_backend(void)
{
    return &tdeck_backend;
}

int picasso_tdeck_init(void)
{
    return tdeck_backend_init();
}

void picasso_tdeck_deinit(void)
{
    tdeck_backend_deinit();
}
