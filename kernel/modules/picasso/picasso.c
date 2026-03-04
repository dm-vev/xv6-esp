/**
 * @file picasso.c
 * @brief Picasso Graphics Server - Core Implementation
 *
 * High-performance graphics server for xv6-esp with T-Deck support.
 * Features:
 * - Hardware-accelerated drawing via ST7789 LCD controller
 * - Double buffering support
 * - Thread-safe operations with spinlocks
 * - Modular backend architecture
 */

#include "picasso.h"
#include "picasso_internal.h"

#include <string.h>
#include <stddef.h>
#include <errno.h>

/* xv6 core headers */
#include "xv6_module.h"
#include "core/spinlock.h"
#include "core/types.h"
#include "core/param.h"
#include "core/riscv.h"

/* Forward declarations */
void initlock(struct spinlock *lk, char *name);
void acquire(struct spinlock *lk);
void release(struct spinlock *lk);

/* ESP-IDF headers for hardware access */
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_st7789.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <hal/spi_hal.h>

/* Module descriptor for kmod system */
#include "../module_manager.h"

/* ============================================================================
 * Internal State
 * ============================================================================ */

/* Global state structure */
typedef struct {
    /* Core state */
    uint8_t initialized;
    uint8_t active;
    uint8_t use_double_buffer;
    uint8_t need_swap;

    /* Display configuration */
    uint16_t width;
    uint16_t height;
    uint8_t bpp;

    /* Framebuffer management */
    uint16_t *framebuffer;      /* Current drawing buffer */
    uint16_t *backbuffer;       /* Back buffer for double buffering */
    uint16_t *displaybuffer;    /* Buffer currently shown */

    /* Backend */
    const picasso_backend_t *backend;
    const picasso_backend_t *backends[PICASSO_MAX_BACKENDS];
    int backend_count;

    /* Clipping region */
    picasso_rect_t clip;

    /* Statistics */
    uint32_t draw_count;
    uint32_t swap_count;
    uint64_t last_flush_us;

    /* Memory */
    uint8_t using_psram;        /* 1 if framebuffers are in PSRAM */

    /* Thread safety */
    struct spinlock lock;
} picasso_state_t;

/* Global state instance */
static picasso_state_t g_state;

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/**
 * @brief Safe color value clamping
 */
static inline uint16_t clamp_color(uint16_t color)
{
    return color & 0xFFFF;
}

/**
 * @brief Clamp coordinate to display bounds
 */
static inline int16_t clamp_x(int16_t x)
{
    if (x < 0) return 0;
    if (x >= g_state.width) return g_state.width - 1;
    return x;
}

/**
 * @brief Clamp coordinate to display bounds
 */
static inline int16_t clamp_y(int16_t y)
{
    if (y < 0) return 0;
    if (y >= g_state.height) return g_state.height - 1;
    return y;
}

/**
 * @brief Check if rectangle is within clipping region
 */
static inline int rect_in_clip(const picasso_rect_t *r)
{
    return (r->x + r->w > g_state.clip.x) &&
           (r->x < g_state.clip.x + g_state.clip.w) &&
           (r->y + r->h > g_state.clip.y) &&
           (r->y < g_state.clip.y + g_state.clip.h);
}

/**
 * @brief Clip rectangle to display bounds
 */
static void clip_rect(picasso_rect_t *r)
{
    int16_t x2 = r->x + r->w;
    int16_t y2 = r->y + r->h;

    r->x = clamp_x(r->x);
    r->y = clamp_y(r->y);
    x2 = clamp_x(x2);
    y2 = clamp_y(y2);

    r->w = x2 - r->x;
    r->h = y2 - r->y;
}

/* ============================================================================
 * Framebuffer Operations
 * ============================================================================ */

/**
 * @brief Initialize framebuffer memory with PSRAM support
 */
static int framebuffer_init(void)
{
    uint32_t size = g_state.width * g_state.height * sizeof(uint16_t);

    /* Allocate framebuffer with PSRAM support (framebuffer doesn't need DMA) */
    g_state.framebuffer = (uint16_t *)picasso_malloc_framebuffer(size, 0);
    if (!g_state.framebuffer) {
        return -1;
    }

    /* Track if using PSRAM */
    g_state.using_psram = psram_available() &&
                          (heap_caps_get_allocated_size(g_state.framebuffer) > 0) &&
                          (psram_get_free_size() < psram_get_size());

    /* Initialize to black */
    memset(g_state.framebuffer, 0, size);
    g_state.displaybuffer = g_state.framebuffer;

    /* Allocate backbuffer if double buffering enabled */
    if (g_state.use_double_buffer) {
        g_state.backbuffer = (uint16_t *)picasso_malloc_framebuffer(size, 0);
        if (!g_state.backbuffer) {
            /* Fall back to single buffer */
            g_state.use_double_buffer = 0;
        } else {
            memset(g_state.backbuffer, 0, size);
        }
    }

    return 0;
}

/**
 * @brief Free framebuffer memory
 */
static void framebuffer_deinit(void)
{
    if (g_state.framebuffer) {
        free(g_state.framebuffer);
        g_state.framebuffer = NULL;
    }
    if (g_state.backbuffer) {
        free(g_state.backbuffer);
        g_state.backbuffer = NULL;
    }
}

/* ============================================================================
 * Drawing Primitives Implementation
 * ============================================================================ */

/**
 * @brief Bresenham line algorithm
 */
static void draw_line_fast(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    int16_t dx = abs(x1 - x0);
    int16_t dy = abs(y1 - y0);
    int16_t sx = (x0 < x1) ? 1 : -1;
    int16_t sy = (y0 < y1) ? 1 : -1;
    int16_t err = dx - dy;
    uint16_t *fb = g_state.framebuffer;

    while (1) {
        if (x0 >= 0 && x0 < g_state.width && y0 >= 0 && y0 < g_state.height) {
            fb[y0 * g_state.width + x0] = color;
        }

        if (x0 == x1 && y0 == y1) break;

        int16_t e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/**
 * @brief Circle drawing using midpoint algorithm
 */
static void draw_circle_fast(int16_t xc, int16_t yc, uint16_t r, uint16_t color, int fill)
{
    int16_t x = 0;
    int16_t y = r;
    int16_t d = 3 - 2 * r;
    uint16_t *fb = g_state.framebuffer;

    while (x <= y) {
        if (fill) {
            /* Draw horizontal lines for filled circle */
            for (int16_t i = xc - x; i <= xc + x; i++) {
                if (i >= 0 && i < g_state.width) {
                    int16_t y1 = yc - y;
                    int16_t y2 = yc + y;
                    if (y1 >= 0 && y1 < g_state.height) fb[y1 * g_state.width + i] = color;
                    if (y2 >= 0 && y2 < g_state.height) fb[y2 * g_state.width + i] = color;
                }
            }
            for (int16_t i = xc - y; i <= xc + y; i++) {
                if (i >= 0 && i < g_state.width) {
                    int16_t y1 = yc - x;
                    int16_t y2 = yc + x;
                    if (y1 >= 0 && y1 < g_state.height) fb[y1 * g_state.width + i] = color;
                    if (y2 >= 0 && y2 < g_state.height) fb[y2 * g_state.width + i] = color;
                }
            }
        } else {
            /* Draw points for outline */
            int16_t points[8][2] = {
                {xc+x, yc+y}, {xc-x, yc+y}, {xc+x, yc-y}, {xc-x, yc-y},
                {xc+y, yc+x}, {xc-y, yc+x}, {xc+y, yc-x}, {xc-y, yc-x}
            };
            for (int i = 0; i < 8; i++) {
                int16_t px = points[i][0];
                int16_t py = points[i][1];
                if (px >= 0 && px < g_state.width && py >= 0 && py < g_state.height) {
                    fb[py * g_state.width + px] = color;
                }
            }
        }

        x++;
        if (d < 0) {
            d = d + 4 * x + 6;
        } else {
            d = d + 4 * (x - y) + 10;
            y--;
        }
    }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

int picasso_init(void)
{
    if (g_state.initialized) {
        return 0;
    }

    /* Initialize state */
    memset(&g_state, 0, sizeof(g_state));
    initlock(&g_state.lock, "picasso");

    /* Default display configuration (T-Deck) */
    g_state.width = PICASSO_DISPLAY_WIDTH;
    g_state.height = PICASSO_DISPLAY_HEIGHT;
    g_state.bpp = PICASSO_BITS_PER_PIXEL;
    g_state.use_double_buffer = 1;

    /* Full screen clip */
    g_state.clip.x = 0;
    g_state.clip.y = 0;
    g_state.clip.w = g_state.width;
    g_state.clip.h = g_state.height;

    /* Initialize framebuffer */
    if (framebuffer_init() < 0) {
        return -1;
    }

    g_state.initialized = 1;
    g_state.active = 1;

    return 0;
}

void picasso_deinit(void)
{
    if (!g_state.initialized) {
        return;
    }

    acquire(&g_state.lock);
    g_state.active = 0;

    /* Deinitialize backend if present */
    if (g_state.backend && g_state.backend->deinit) {
        g_state.backend->deinit();
    }

    framebuffer_deinit();
    g_state.initialized = 0;
    release(&g_state.lock);
}

int picasso_get_display_info(picasso_display_info_t *info)
{
    if (!info) {
        errno = EINVAL;
        return -1;
    }

    acquire(&g_state.lock);
    info->width = g_state.width;
    info->height = g_state.height;
    info->bpp = g_state.bpp;
    info->initialized = g_state.initialized;
    info->double_buffer = g_state.use_double_buffer;
    release(&g_state.lock);

    return 0;
}

int picasso_register_backend(const picasso_backend_t *backend)
{
    if (!backend || !backend->name || !backend->init) {
        errno = EINVAL;
        return -1;
    }

    acquire(&g_state.lock);

    if (g_state.backend_count >= PICASSO_MAX_BACKENDS) {
        release(&g_state.lock);
        errno = ENOSPC;
        return -1;
    }

    /* Check for duplicate names */
    for (int i = 0; i < g_state.backend_count; i++) {
        if (strcmp(g_state.backends[i]->name, backend->name) == 0) {
            release(&g_state.lock);
            errno = EEXIST;
            return -1;
        }
    }

    g_state.backends[g_state.backend_count++] = backend;
    release(&g_state.lock);

    return 0;
}

int picasso_select_backend(const char *name)
{
    if (!name) {
        errno = EINVAL;
        return -1;
    }

    acquire(&g_state.lock);

    /* Find backend by name */
    const picasso_backend_t *new_backend = NULL;
    for (int i = 0; i < g_state.backend_count; i++) {
        if (strcmp(g_state.backends[i]->name, name) == 0) {
            new_backend = g_state.backends[i];
            break;
        }
    }

    if (!new_backend) {
        release(&g_state.lock);
        errno = ENOENT;
        return -1;
    }

    /* Deinitialize old backend if any */
    if (g_state.backend && g_state.backend->deinit) {
        g_state.backend->deinit();
    }

    /* Initialize new backend */
    if (new_backend->init() < 0) {
        release(&g_state.lock);
        errno = EIO;
        return -1;
    }

    g_state.backend = new_backend;
    release(&g_state.lock);

    return 0;
}

int picasso_set_backlight(uint8_t level)
{
    acquire(&g_state.lock);

    if (!g_state.backend || !g_state.backend->set_backlight) {
        release(&g_state.lock);
        errno = ENOSYS;
        return -1;
    }

    int ret = g_state.backend->set_backlight(level);
    release(&g_state.lock);

    return ret;
}

int picasso_clear(uint16_t color)
{
    acquire(&g_state.lock);

    if (!g_state.initialized) {
        release(&g_state.lock);
        errno = ENODEV;
        return -1;
    }

    uint32_t size = g_state.width * g_state.height * sizeof(uint16_t);
    uint16_t *buf = g_state.framebuffer;

    /* Fast fill using 32-bit writes where possible */
    uint32_t color32 = (color << 16) | color;
    uint32_t *buf32 = (uint32_t *)buf;
    uint32_t count = size / 4;

    for (uint32_t i = 0; i < count; i++) {
        buf32[i] = color32;
    }

    /* Handle any remaining bytes */
    if (size & 2) {
        buf[size / 2 - 1] = color;
    }

    g_state.draw_count++;
    release(&g_state.lock);

    return 0;
}

int picasso_draw_pixel(int16_t x, int16_t y, uint16_t color)
{
    if (x < 0 || x >= g_state.width || y < 0 || y >= g_state.height) {
        return 0; /* Clipped */
    }

    acquire(&g_state.lock);
    g_state.framebuffer[y * g_state.width + x] = color;
    g_state.draw_count++;
    release(&g_state.lock);

    return 0;
}

int picasso_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    acquire(&g_state.lock);

    if (!g_state.initialized) {
        release(&g_state.lock);
        errno = ENODEV;
        return -1;
    }

    draw_line_fast(x0, y0, x1, y1, color);
    g_state.draw_count++;
    release(&g_state.lock);

    return 0;
}

int picasso_draw_rect(const picasso_rect_t *rect, uint16_t color)
{
    if (!rect) {
        errno = EINVAL;
        return -1;
    }

    acquire(&g_state.lock);

    if (!g_state.initialized) {
        release(&g_state.lock);
        errno = ENODEV;
        return -1;
    }

    picasso_rect_t r = *rect;
    clip_rect(&r);

    if (r.w == 0 || r.h == 0) {
        release(&g_state.lock);
        return 0;
    }

    /* Draw outline: top, bottom, left, right */
    int16_t x2 = r.x + r.w - 1;
    int16_t y2 = r.y + r.h - 1;

    draw_line_fast(r.x, r.y, x2, r.y, color);      /* Top */
    draw_line_fast(r.x, y2, x2, y2, color);      /* Bottom */
    draw_line_fast(r.x, r.y, r.x, y2, color);      /* Left */
    draw_line_fast(x2, r.y, x2, y2, color);        /* Right */

    g_state.draw_count++;
    release(&g_state.lock);

    return 0;
}

int picasso_fill_rect(const picasso_rect_t *rect, uint16_t color)
{
    if (!rect) {
        errno = EINVAL;
        return -1;
    }

    acquire(&g_state.lock);

    if (!g_state.initialized) {
        release(&g_state.lock);
        errno = ENODEV;
        return -1;
    }

    picasso_rect_t r = *rect;
    clip_rect(&r);

    if (r.w == 0 || r.h == 0) {
        release(&g_state.lock);
        return 0;
    }

    uint16_t *fb = g_state.framebuffer;
    int16_t row_stride = g_state.width - r.w;

    for (int16_t y = r.y; y < r.y + r.h; y++) {
        uint16_t *row = &fb[y * g_state.width + r.x];
        for (int16_t x = 0; x < r.w; x++) {
            row[x] = color;
        }
    }

    g_state.draw_count++;
    release(&g_state.lock);

    return 0;
}

int picasso_draw_circle(const picasso_point_t *center, uint16_t radius, uint16_t color)
{
    if (!center) {
        errno = EINVAL;
        return -1;
    }

    acquire(&g_state.lock);

    if (!g_state.initialized) {
        release(&g_state.lock);
        errno = ENODEV;
        return -1;
    }

    draw_circle_fast(center->x, center->y, radius, color, 0);
    g_state.draw_count++;
    release(&g_state.lock);

    return 0;
}

int picasso_fill_circle(const picasso_point_t *center, uint16_t radius, uint16_t color)
{
    if (!center) {
        errno = EINVAL;
        return -1;
    }

    acquire(&g_state.lock);

    if (!g_state.initialized) {
        release(&g_state.lock);
        errno = ENODEV;
        return -1;
    }

    draw_circle_fast(center->x, center->y, radius, color, 1);
    g_state.draw_count++;
    release(&g_state.lock);

    return 0;
}

int picasso_draw_bitmap(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *data)
{
    if (!data) {
        errno = EINVAL;
        return -1;
    }

    acquire(&g_state.lock);

    if (!g_state.initialized) {
        release(&g_state.lock);
        errno = ENODEV;
        return -1;
    }

    /* Calculate clipping */
    int16_t x1 = x;
    int16_t y1 = y;
    int16_t x2 = x + w;
    int16_t y2 = y + h;

    int16_t src_x = 0;
    int16_t src_y = 0;

    if (x1 < 0) { src_x = -x1; x1 = 0; }
    if (y1 < 0) { src_y = -y1; y1 = 0; }
    if (x2 > g_state.width) x2 = g_state.width;
    if (y2 > g_state.height) y2 = g_state.height;

    int16_t dst_w = x2 - x1;
    int16_t dst_h = y2 - y1;

    if (dst_w <= 0 || dst_h <= 0) {
        release(&g_state.lock);
        return 0;
    }

    /* Copy bitmap data */
    uint16_t *fb = g_state.framebuffer;
    for (int16_t row = 0; row < dst_h; row++) {
        uint16_t *dst = &fb[(y1 + row) * g_state.width + x1];
        const uint16_t *src = &data[(src_y + row) * w + src_x];
        memcpy(dst, src, dst_w * sizeof(uint16_t));
    }

    g_state.draw_count++;
    release(&g_state.lock);

    return 0;
}

int picasso_swap_buffers(void)
{
    acquire(&g_state.lock);

    if (!g_state.initialized) {
        release(&g_state.lock);
        errno = ENODEV;
        return -1;
    }

    if (!g_state.use_double_buffer || !g_state.backbuffer) {
        release(&g_state.lock);
        return 0;
    }

    /* Swap front and back buffers */
    uint16_t *temp = g_state.framebuffer;
    g_state.framebuffer = g_state.backbuffer;
    g_state.backbuffer = temp;
    g_state.need_swap = 1;
    g_state.swap_count++;

    release(&g_state.lock);
    return 0;
}

void picasso_wait_done(void)
{
    if (g_state.backend && g_state.backend->wait_done) {
        g_state.backend->wait_done();
    }
}

uint16_t* picasso_get_framebuffer(void)
{
    /* This is unsafe and should be used with caution */
    return g_state.framebuffer;
}

int picasso_flush(void)
{
    acquire(&g_state.lock);

    if (!g_state.initialized || !g_state.backend) {
        release(&g_state.lock);
        errno = ENODEV;
        return -1;
    }

    if (!g_state.backend->draw_bitmap) {
        release(&g_state.lock);
        errno = ENOSYS;
        return -1;
    }

    /* Send current display buffer to hardware */
    int ret = g_state.backend->draw_bitmap(
        0, 0, g_state.width, g_state.height,
        g_state.displaybuffer
    );

    if (ret == 0) {
        g_state.need_swap = 0;

        /* If using double buffering, update display pointer */
        if (g_state.use_double_buffer) {
            g_state.displaybuffer = g_state.framebuffer;
        }
    }

    release(&g_state.lock);
    return ret;
}

/* ============================================================================
 * PSRAM Memory Management Implementation
 * ============================================================================ */

int picasso_psram_available(void)
{
    return psram_available();
}

uint32_t picasso_psram_get_size(void)
{
    return psram_get_size();
}

uint32_t picasso_psram_get_free(void)
{
    return psram_get_free_size();
}

uint32_t picasso_internal_get_free(void)
{
    return internal_ram_get_free_size();
}

int picasso_using_psram(void)
{
    return g_state.using_psram;
}

int picasso_psram_config(int use_psram, uint32_t threshold)
{
    acquire(&g_state.lock);

    /* Only allow reconfiguration if not initialized */
    if (g_state.initialized) {
        release(&g_state.lock);
        errno = EBUSY;
        return -1;
    }

    /* Note: Actual configuration is compile-time via PICASSO_USE_PSRAM
     * This function could be extended to use runtime configuration */

    release(&g_state.lock);
    return 0;
}

int picasso_get_memory_info(uint32_t *internal_free, uint32_t *psram_free,
                               uint32_t *framebuffer_size, int *using_psram)
{
    if (!internal_free || !psram_free || !framebuffer_size || !using_psram) {
        errno = EINVAL;
        return -1;
    }

    acquire(&g_state.lock);

    *internal_free = internal_ram_get_free_size();
    *psram_free = psram_get_free_size();
    *framebuffer_size = g_state.width * g_state.height * sizeof(uint16_t);
    *using_psram = g_state.using_psram;

    release(&g_state.lock);
    return 0;
}

void picasso_get_memory_stats(picasso_mem_stats_t *stats)
{
    if (!stats) {
        return;
    }

    acquire(&g_state.lock);

    stats->internal_total = 0; /* Not tracked */
    stats->internal_free = internal_ram_get_free_size();
    stats->psram_total = psram_get_size();
    stats->psram_free = psram_get_free_size();
    stats->framebuffer_size = g_state.width * g_state.height * sizeof(uint16_t);
    stats->using_psram = g_state.using_psram;

    release(&g_state.lock);
}

/* ============================================================================
 * Module Interface
 * ============================================================================ */

static int xv6_module_init(void)
{
    int ret = picasso_init();
    if (ret < 0) {
        return ret;
    }

    /* Register T-Deck backend if available */
    extern const picasso_backend_t *picasso_tdeck_get_backend(void);
    const picasso_backend_t *tdeck_backend = picasso_tdeck_get_backend();
    if (tdeck_backend) {
        picasso_register_backend(tdeck_backend);
        picasso_select_backend("tdeck");
    }

    return 0;
}

static int xv6_module_fini(void)
{
    picasso_deinit();
    return 0;
}

/* Exported symbols */
static const xv6_module_symbol_t g_symbols[] = {
    /* Core API */
    { "picasso_init", (void *)picasso_init, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_deinit", (void *)picasso_deinit, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_get_display_info", (void *)picasso_get_display_info, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_register_backend", (void *)picasso_register_backend, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_select_backend", (void *)picasso_select_backend, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_set_backlight", (void *)picasso_set_backlight, XV6_MODULE_SYMBOL_EXTENSION, 50 },

    /* Drawing API */
    { "picasso_clear", (void *)picasso_clear, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_draw_pixel", (void *)picasso_draw_pixel, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_draw_line", (void *)picasso_draw_line, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_draw_rect", (void *)picasso_draw_rect, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_fill_rect", (void *)picasso_fill_rect, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_draw_circle", (void *)picasso_draw_circle, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_fill_circle", (void *)picasso_fill_circle, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_draw_bitmap", (void *)picasso_draw_bitmap, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_swap_buffers", (void *)picasso_swap_buffers, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_wait_done", (void *)picasso_wait_done, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_get_framebuffer", (void *)picasso_get_framebuffer, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_flush", (void *)picasso_flush, XV6_MODULE_SYMBOL_EXTENSION, 50 },

    /* PSRAM Memory API */
    { "picasso_psram_available", (void *)picasso_psram_available, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_psram_get_size", (void *)picasso_psram_get_size, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_psram_get_free", (void *)picasso_psram_get_free, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_internal_get_free", (void *)picasso_internal_get_free, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_using_psram", (void *)picasso_using_psram, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_psram_config", (void *)picasso_psram_config, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_get_memory_info", (void *)picasso_get_memory_info, XV6_MODULE_SYMBOL_EXTENSION, 50 },
    { "picasso_get_memory_stats", (void *)picasso_get_memory_stats, XV6_MODULE_SYMBOL_EXTENSION, 50 },
};

static const xv6_module_desc_t g_desc = {
    KMOD_MODULE_ABI_VER,
    "picasso",
    50,
    g_symbols,
    (int)(sizeof(g_symbols) / sizeof(g_symbols[0])),
};

const xv6_module_desc_t *xv6_module_describe(void)
{
    return &g_desc;
}
