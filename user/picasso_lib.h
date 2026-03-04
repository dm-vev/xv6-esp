/**
 * @file picasso_lib.h
 * @brief Picasso Graphics Library - User Space Interface
 *
 * Library for user programs to interface with picasso kernel module.
 * Provides drawing primitives, buffer management, and display control.
 */

#ifndef PICASSO_LIB_H
#define PICASSO_LIB_H

#include <stdint.h>
#include <stdbool.h>

/* Display configuration */
#define PICASSO_DISPLAY_WIDTH   320
#define PICASSO_DISPLAY_HEIGHT  240

/* Color format: BGR565 */
#define PICASSO_COLOR_B565(r, g, b) \
    ((((b) & 0x1F) << 11) | (((g) & 0x3F) << 5) | ((r) & 0x1F))

/* Predefined colors */
#define PICASSO_COLOR_RED       0xF800
#define PICASSO_COLOR_GREEN     0x07E0
#define PICASSO_COLOR_BLUE      0x001F
#define PICASSO_COLOR_WHITE     0xFFFF
#define PICASSO_COLOR_BLACK     0x0000
#define PICASSO_COLOR_YELLOW    0xFFE0
#define PICASSO_COLOR_CYAN      0x07FF
#define PICASSO_COLOR_MAGENTA   0xF81F

/* Error codes */
#define PICASSO_SUCCESS         0
#define PICASSO_ERROR_INIT     -1
#define PICASSO_ERROR_IO       -2
#define PICASSO_ERROR_NOMEM    -3
#define PICASSO_ERROR_INVALID  -4

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
} picasso_info_t;

/* ============================================================================
 * Initialization
 * ============================================================================ */

/**
 * @brief Initialize picasso library and connect to kernel module
 * @return PICASSO_SUCCESS on success, error code otherwise
 */
int picasso_open(void);

/**
 * @brief Close picasso library connection
 */
void picasso_close(void);

/**
 * @brief Get display information
 * @param info Pointer to info structure to fill
 * @return 0 on success, -1 on error
 */
int picasso_get_info(picasso_info_t *info);

/* ============================================================================
 * Display Control
 * ============================================================================ */

/**
 * @brief Set backlight level
 * @param level 0-255, where 0 is off and 255 is full brightness
 * @return 0 on success
 */
int picasso_set_backlight(uint8_t level);

/**
 * @brief Clear screen with color
 * @param color BGR565 color value
 * @return 0 on success
 */
int picasso_clear(uint16_t color);

/**
 * @brief Swap front/back buffers (if double buffering enabled)
 * @return 0 on success
 */
int picasso_swap(void);

/**
 * @brief Flush pending changes to display
 * @return 0 on success
 */
int picasso_flush(void);

/* ============================================================================
 * Drawing Functions
 * ============================================================================ */

/**
 * @brief Draw pixel
 * @param x X coordinate
 * @param y Y coordinate
 * @param color BGR565 color
 * @return 0 on success
 */
int picasso_pixel(int16_t x, int16_t y, uint16_t color);

/**
 * @brief Draw line
 * @param x0 Start X
 * @param y0 Start Y
 * @param x1 End X
 * @param y1 End Y
 * @param color BGR565 color
 * @return 0 on success
 */
int picasso_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);

/**
 * @brief Draw rectangle outline
 * @param rect Rectangle coordinates
 * @param color BGR565 color
 * @return 0 on success
 */
int picasso_rect(const picasso_rect_t *rect, uint16_t color);

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
int picasso_circle(const picasso_point_t *center, uint16_t radius, uint16_t color);

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
int picasso_bitmap(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *data);

/* ============================================================================
 * Framebuffer Access
 * ============================================================================ */

/**
 * @brief Map framebuffer into process address space
 * @return Pointer to framebuffer or NULL on error
 */
uint16_t* picasso_map_framebuffer(void);

/**
 * @brief Unmap framebuffer
 */
void picasso_unmap_framebuffer(void);

/**
 * @brief Get framebuffer dimensions
 * @param width Output width
 * @param height Output height
 * @return 0 on success
 */
int picasso_get_framebuffer_size(uint16_t *width, uint16_t *height);

#endif /* PICASSO_LIB_H */
