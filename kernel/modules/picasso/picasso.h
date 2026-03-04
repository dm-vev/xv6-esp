#ifndef PICASSO_H
#define PICASSO_H

#include <stdint.h>
#include <stdbool.h>

/* Picasso Graphics Server - Kernel Module API
 * High-performance graphics server with T-Deck backend support
 */

/* Display configuration for T-Deck */
#define PICASSO_DISPLAY_WIDTH   320
#define PICASSO_DISPLAY_HEIGHT  240
#define PICASSO_BITS_PER_PIXEL  16
#define PICASSO_BUFFER_SIZE     (PICASSO_DISPLAY_WIDTH * PICASSO_DISPLAY_HEIGHT * 2)

/* Color format: BGR565 (ST7789 native) */
#define PICASSO_COLOR_B565(r, g, b) \
    ((((b) & 0x1F) << 11) | (((g) & 0x3F) << 5) | ((r) & 0x1F))

/* Predefined colors (BGR565, byte-swapped for little-endian) */
#define PICASSO_COLOR_RED       0xF800
#define PICASSO_COLOR_GREEN     0x07E0
#define PICASSO_COLOR_BLUE      0x001F
#define PICASSO_COLOR_WHITE     0xFFFF
#define PICASSO_COLOR_BLACK     0x0000
#define PICASSO_COLOR_YELLOW    0xFFE0
#define PICASSO_COLOR_CYAN      0x07FF
#define PICASSO_COLOR_MAGENTA   0xF81F
#define PICASSO_COLOR_GRAY      0x8410
#define PICASSO_COLOR_DARK_GRAY 0x4208

/* Drawing operations */
typedef enum {
    PICASSO_OP_NONE = 0,
    PICASSO_OP_PIXEL,
    PICASSO_OP_LINE,
    PICASSO_OP_RECT,
    PICASSO_OP_FILL_RECT,
    PICASSO_OP_CIRCLE,
    PICASSO_OP_FILL_CIRCLE,
    PICASSO_OP_BITMAP,
    PICASSO_OP_TEXT,
    PICASSO_OP_CLEAR,
    PICASSO_OP_SWAP,      /* Swap front/back buffer */
} picasso_op_t;

/* Rectangle structure */
typedef struct {
    int16_t x;
    int16_t y;
    uint16_t w;
    uint16_t h;
} picasso_rect_t;

/* Point structure */
typedef struct {
    int16_t x;
    int16_t y;
} picasso_point_t;

/* Display info */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t bpp;
    uint8_t initialized;
    uint8_t double_buffer;  /* 1 if using double buffering */
} picasso_display_info_t;

/* Backend interface - hardware abstraction */
typedef struct picasso_backend {
    const char *name;
    int (*init)(void);
    void (*deinit)(void);
    int (*set_backlight)(uint8_t level);  /* 0-255 */
    int (*draw_bitmap)(int16_t x1, int16_t y1, int16_t x2, int16_t y2, const void *data);
    void (*wait_done)(void);
    int (*set_power)(bool on);
} picasso_backend_t;

/* ============================================================================
 * Core API Functions
 * ============================================================================ */

/**
 * @brief Initialize the graphics server
 * @return 0 on success, -1 on error
 */
int picasso_init(void);

/**
 * @brief Deinitialize the graphics server
 */
void picasso_deinit(void);

/**
 * @brief Get display information
 * @param info Pointer to display info structure
 * @return 0 on success
 */
int picasso_get_display_info(picasso_display_info_t *info);

/**
 * @brief Register a backend (typically called by backend itself)
 * @param backend Pointer to backend structure
 * @return 0 on success
 */
int picasso_register_backend(const picasso_backend_t *backend);

/**
 * @brief Select active backend
 * @param name Backend name
 * @return 0 on success
 */
int picasso_select_backend(const char *name);

/**
 * @brief Set backlight level
 * @param level 0-255
 * @return 0 on success
 */
int picasso_set_backlight(uint8_t level);

/* ============================================================================
 * Drawing API
 * ============================================================================ */

/**
 * @brief Clear screen with color
 * @param color BGR565 color value
 * @return 0 on success
 */
int picasso_clear(uint16_t color);

/**
 * @brief Draw single pixel
 * @param x X coordinate
 * @param y Y coordinate
 * @param color BGR565 color
 * @return 0 on success
 */
int picasso_draw_pixel(int16_t x, int16_t y, uint16_t color);

/**
 * @brief Draw line
 * @param x0 Start X
 * @param y0 Start Y
 * @param x1 End X
 * @param y1 End Y
 * @param color BGR565 color
 * @return 0 on success
 */
int picasso_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);

/**
 * @brief Draw rectangle outline
 * @param rect Rectangle coordinates
 * @param color BGR565 color
 * @return 0 on success
 */
int picasso_draw_rect(const picasso_rect_t *rect, uint16_t color);

/**
 * @brief Draw filled rectangle
 * @param rect Rectangle coordinates
 * @param color BGR565 color
 * @return 0 on success
 */
int picasso_fill_rect(const picasso_rect_t *rect, uint16_t color);

/**
 * @brief Draw circle outline
 * @param center Center point
 * @param radius Radius in pixels
 * @param color BGR565 color
 * @return 0 on success
 */
int picasso_draw_circle(const picasso_point_t *center, uint16_t radius, uint16_t color);

/**
 * @brief Draw filled circle
 * @param center Center point
 * @param radius Radius in pixels
 * @param color BGR565 color
 * @return 0 on success
 */
int picasso_fill_circle(const picasso_point_t *center, uint16_t radius, uint16_t color);

/**
 * @brief Draw bitmap
 * @param x X position
 * @param y Y position
 * @param w Width
 * @param h Height
 * @param data Pointer to BGR565 bitmap data
 * @return 0 on success
 */
int picasso_draw_bitmap(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *data);

/**
 * @brief Swap buffers (for double buffering)
 * @return 0 on success
 */
int picasso_swap_buffers(void);

/**
 * @brief Wait for all pending drawing operations to complete
 */
void picasso_wait_done(void);

/**
 * @brief Direct buffer access (unsafe, for advanced use)
 * @return Pointer to framebuffer or NULL if not available
 */
uint16_t* picasso_get_framebuffer(void);

/**
 * @brief Flush framebuffer to display
 * @return 0 on success
 */
int picasso_flush(void);

/* ============================================================================
 * T-Deck Specific API
 * ============================================================================ */

/**
 * @brief Initialize T-Deck backend
 * @return 0 on success
 */
int picasso_tdeck_init(void);

/**
 * @brief Deinitialize T-Deck backend
 */
void picasso_tdeck_deinit(void);

/**
 * @brief Get T-Deck backend structure
 * @return Pointer to backend
 */
const picasso_backend_t* picasso_tdeck_get_backend(void);

/* ============================================================================
 * PSRAM Memory Management API
 * ============================================================================ */

/**
 * @brief Check if PSRAM is available
 * @return 1 if PSRAM is available and enabled, 0 otherwise
 */
int picasso_psram_available(void);

/**
 * @brief Get PSRAM size
 * @return Total PSRAM size in bytes, 0 if not available
 */
uint32_t picasso_psram_get_size(void);

/**
 * @brief Get free PSRAM
 * @return Free PSRAM in bytes, 0 if not available
 */
uint32_t picasso_psram_get_free(void);

/**
 * @brief Get free internal RAM
 * @return Free internal RAM in bytes
 */
uint32_t picasso_internal_get_free(void);

/**
 * @brief Check if framebuffers are using PSRAM
 * @return 1 if using PSRAM, 0 if using internal RAM
 */
int picasso_using_psram(void);

/**
 * @brief Configure PSRAM usage policy
 * @param use_psram 1 to enable PSRAM, 0 to disable
 * @param threshold Minimum internal RAM before using PSRAM (in KB), 0 for default
 * @return 0 on success, -1 on error
 */
int picasso_psram_config(int use_psram, uint32_t threshold);

/**
 * @brief Get memory usage statistics
 * @param internal_free Output: free internal RAM in bytes
 * @param psram_free Output: free PSRAM in bytes
 * @param framebuffer_size Output: size of each framebuffer in bytes
 * @param using_psram Output: 1 if framebuffers in PSRAM, 0 otherwise
 * @return 0 on success, -1 on error
 */
int picasso_get_memory_info(uint32_t *internal_free, uint32_t *psram_free,
                               uint32_t *framebuffer_size, int *using_psram);

#endif /* PICASSO_H */
