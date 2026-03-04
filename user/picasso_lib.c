/**
 * @file picasso_lib.c
 * @brief Picasso Graphics Library - User Space Implementation
 */

#include "picasso_lib.h"
#include "kernel/core/types.h"
#include "kernel/core/syscall.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

/* Device path */
#define PICASSO_DEVICE "/dev/picasso"

/* Internal state */
static struct {
    int fd;
    int initialized;
    uint16_t *framebuffer;
    size_t fb_size;
    uint16_t width;
    uint16_t height;
} g_state;

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static inline int check_init(void)
{
    if (!g_state.initialized) {
        return PICASSO_ERROR_INIT;
    }
    return PICASSO_SUCCESS;
}

/* ============================================================================
 * Initialization
 * ============================================================================ */

int picasso_open(void)
{
    if (g_state.initialized) {
        return PICASSO_SUCCESS;
    }

    /* Open device file */
    g_state.fd = open(PICASSO_DEVICE, O_RDWR);
    if (g_state.fd < 0) {
        return PICASSO_ERROR_IO;
    }

    /* Get display info */
    picasso_info_t info;
    if (picasso_get_info(&info) < 0) {
        close(g_state.fd);
        g_state.fd = -1;
        return PICASSO_ERROR_IO;
    }

    g_state.width = info.width;
    g_state.height = info.height;
    g_state.fb_size = g_state.width * g_state.height * sizeof(uint16_t);
    g_state.initialized = 1;

    return PICASSO_SUCCESS;
}

void picasso_close(void)
{
    if (!g_state.initialized) {
        return;
    }

    if (g_state.framebuffer) {
        picasso_unmap_framebuffer();
    }

    if (g_state.fd >= 0) {
        close(g_state.fd);
        g_state.fd = -1;
    }

    g_state.initialized = 0;
}

int picasso_get_info(picasso_info_t *info)
{
    if (!info) {
        return PICASSO_ERROR_INVALID;
    }

    if (!g_state.initialized) {
        return PICASSO_ERROR_INIT;
    }

    /* Use ioctl to get info */
    if (ioctl(g_state.fd, 0, info) < 0) {
        return PICASSO_ERROR_IO;
    }

    return PICASSO_SUCCESS;
}

/* ============================================================================
 * Display Control
 * ============================================================================ */

int picasso_set_backlight(uint8_t level)
{
    if (check_init() != PICASSO_SUCCESS) {
        return PICASSO_ERROR_INIT;
    }

    /* Use ioctl for backlight control */
    if (ioctl(g_state.fd, 1, &level) < 0) {
        return PICASSO_ERROR_IO;
    }

    return PICASSO_SUCCESS;
}

int picasso_clear(uint16_t color)
{
    if (check_init() != PICASSO_SUCCESS) {
        return PICASSO_ERROR_INIT;
    }

    picasso_rect_t rect = {
        .x = 0,
        .y = 0,
        .w = g_state.width,
        .h = g_state.height
    };

    return picasso_fill_rect(&rect, color);
}

int picasso_swap(void)
{
    if (check_init() != PICASSO_SUCCESS) {
        return PICASSO_ERROR_INIT;
    }

    /* ioctl for buffer swap */
    if (ioctl(g_state.fd, 2, NULL) < 0) {
        return PICASSO_ERROR_IO;
    }

    return PICASSO_SUCCESS;
}

int picasso_flush(void)
{
    if (check_init() != PICASSO_SUCCESS) {
        return PICASSO_ERROR_INIT;
    }

    /* ioctl for flush */
    if (ioctl(g_state.fd, 3, NULL) < 0) {
        return PICASSO_ERROR_IO;
    }

    return PICASSO_SUCCESS;
}

/* ============================================================================
 * Drawing Functions - Software Implementation
 * ============================================================================
 *
 * These implementations draw directly to the mapped framebuffer.
 * If framebuffer is not mapped, they fall back to ioctl calls.
 */

int picasso_pixel(int16_t x, int16_t y, uint16_t color)
{
    if (check_init() != PICASSO_SUCCESS) {
        return PICASSO_ERROR_INIT;
    }

    if (x < 0 || x >= g_state.width || y < 0 || y >= g_state.height) {
        return PICASSO_SUCCESS; /* Clipped */
    }

    if (g_state.framebuffer) {
        g_state.framebuffer[y * g_state.width + x] = color;
        return PICASSO_SUCCESS;
    }

    /* Fall back to ioctl */
    struct {
        int16_t x;
        int16_t y;
        uint16_t color;
    } args = { x, y, color };

    if (ioctl(g_state.fd, 10, &args) < 0) {
        return PICASSO_ERROR_IO;
    }

    return PICASSO_SUCCESS;
}

/* Bresenham line algorithm */
int picasso_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color)
{
    if (check_init() != PICASSO_SUCCESS) {
        return PICASSO_ERROR_INIT;
    }

    if (g_state.framebuffer) {
        int16_t dx = abs(x1 - x0);
        int16_t dy = abs(y1 - y0);
        int16_t sx = (x0 < x1) ? 1 : -1;
        int16_t sy = (y0 < y1) ? 1 : -1;
        int16_t err = dx - dy;

        while (1) {
            if (x0 >= 0 && x0 < g_state.width && y0 >= 0 && y0 < g_state.height) {
                g_state.framebuffer[y0 * g_state.width + x0] = color;
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
        return PICASSO_SUCCESS;
    }

    /* Fall back to ioctl batch operation */
    struct {
        int16_t x0, y0, x1, y1;
        uint16_t color;
    } args = { x0, y0, x1, y1, color };

    if (ioctl(g_state.fd, 11, &args) < 0) {
        return PICASSO_ERROR_IO;
    }

    return PICASSO_SUCCESS;
}

int picasso_rect(const picasso_rect_t *rect, uint16_t color)
{
    if (check_init() != PICASSO_SUCCESS) {
        return PICASSO_ERROR_INIT;
    }

    if (!rect) {
        return PICASSO_ERROR_INVALID;
    }

    /* Draw rectangle outline using four lines */
    int16_t x2 = rect->x + rect->w - 1;
    int16_t y2 = rect->y + rect->h - 1;

    picasso_line(rect->x, rect->y, x2, rect->y, color);      /* Top */
    picasso_line(rect->x, y2, x2, y2, color);                /* Bottom */
    picasso_line(rect->x, rect->y, rect->x, y2, color);      /* Left */
    picasso_line(x2, rect->y, x2, y2, color);                  /* Right */

    return PICASSO_SUCCESS;
}

int picasso_fill_rect(const picasso_rect_t *rect, uint16_t color)
{
    if (check_init() != PICASSO_SUCCESS) {
        return PICASSO_ERROR_INIT;
    }

    if (!rect) {
        return PICASSO_ERROR_INVALID;
    }

    /* Clip to display bounds */
    int16_t x = rect->x;
    int16_t y = rect->y;
    int16_t w = rect->w;
    int16_t h = rect->h;

    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_state.width) w = g_state.width - x;
    if (y + h > g_state.height) h = g_state.height - y;

    if (w <= 0 || h <= 0) {
        return PICASSO_SUCCESS;
    }

    if (g_state.framebuffer) {
        for (int16_t row = 0; row < h; row++) {
            uint16_t *line = &g_state.framebuffer[(y + row) * g_state.width + x];
            for (int16_t col = 0; col < w; col++) {
                line[col] = color;
            }
        }
        return PICASSO_SUCCESS;
    }

    /* Fall back to ioctl */
    struct {
        int16_t x, y, w, h;
        uint16_t color;
    } args = { x, y, w, h, color };

    if (ioctl(g_state.fd, 12, &args) < 0) {
        return PICASSO_ERROR_IO;
    }

    return PICASSO_SUCCESS;
}

/* Circle using midpoint algorithm */
int picasso_circle(const picasso_point_t *center, uint16_t radius, uint16_t color)
{
    if (check_init() != PICASSO_SUCCESS) {
        return PICASSO_ERROR_INIT;
    }

    if (!center) {
        return PICASSO_ERROR_INVALID;
    }

    if (g_state.framebuffer) {
        int16_t x = 0;
        int16_t y = radius;
        int16_t d = 3 - 2 * radius;

        while (x <= y) {
            /* Draw 8 points for each octant */
            picasso_pixel(center->x + x, center->y + y, color);
            picasso_pixel(center->x - x, center->y + y, color);
            picasso_pixel(center->x + x, center->y - y, color);
            picasso_pixel(center->x - x, center->y - y, color);
            picasso_pixel(center->x + y, center->y + x, color);
            picasso_pixel(center->x - y, center->y + x, color);
            picasso_pixel(center->x + y, center->y - x, color);
            picasso_pixel(center->x - y, center->y - x, color);

            x++;
            if (d < 0) {
                d = d + 4 * x + 6;
            } else {
                d = d + 4 * (x - y) + 10;
                y--;
            }
        }
        return PICASSO_SUCCESS;
    }

    /* Fall back to ioctl */
    struct {
        int16_t cx, cy;
        uint16_t radius;
        uint16_t color;
    } args = { center->x, center->y, radius, color };

    if (ioctl(g_state.fd, 13, &args) < 0) {
        return PICASSO_ERROR_IO;
    }

    return PICASSO_SUCCESS;
}

int picasso_fill_circle(const picasso_point_t *center, uint16_t radius, uint16_t color)
{
    if (check_init() != PICASSO_SUCCESS) {
        return PICASSO_ERROR_INIT;
    }

    if (!center) {
        return PICASSO_ERROR_INVALID;
    }

    if (g_state.framebuffer) {
        int16_t x = 0;
        int16_t y = radius;
        int16_t d = 3 - 2 * radius;

        while (x <= y) {
            /* Draw horizontal lines */
            for (int16_t i = center->x - x; i <= center->x + x; i++) {
                picasso_pixel(i, center->y - y, color);
                picasso_pixel(i, center->y + y, color);
            }
            for (int16_t i = center->x - y; i <= center->x + y; i++) {
                picasso_pixel(i, center->y - x, color);
                picasso_pixel(i, center->y + x, color);
            }

            x++;
            if (d < 0) {
                d = d + 4 * x + 6;
            } else {
                d = d + 4 * (x - y) + 10;
                y--;
            }
        }
        return PICASSO_SUCCESS;
    }

    /* Fall back to ioctl */
    struct {
        int16_t cx, cy;
        uint16_t radius;
        uint16_t color;
        uint8_t fill;
    } args = { center->x, center->y, radius, color, 1 };

    if (ioctl(g_state.fd, 14, &args) < 0) {
        return PICASSO_ERROR_IO;
    }

    return PICASSO_SUCCESS;
}

int picasso_bitmap(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *data)
{
    if (check_init() != PICASSO_SUCCESS) {
        return PICASSO_ERROR_INIT;
    }

    if (!data) {
        return PICASSO_ERROR_INVALID;
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
        return PICASSO_SUCCESS;
    }

    if (g_state.framebuffer) {
        for (int16_t row = 0; row < dst_h; row++) {
            uint16_t *dst = &g_state.framebuffer[(y1 + row) * g_state.width + x1];
            const uint16_t *src = &data[(src_y + row) * w + src_x];
            for (int16_t col = 0; col < dst_w; col++) {
                dst[col] = src[col];
            }
        }
        return PICASSO_SUCCESS;
    }

    /* Fall back to ioctl with bitmap data */
    /* This would require a more complex ioctl to pass the bitmap */
    return PICASSO_ERROR_IO;
}

/* ============================================================================
 * Framebuffer Access
 * ============================================================================ */

uint16_t* picasso_map_framebuffer(void)
{
    if (check_init() != PICASSO_SUCCESS) {
        return NULL;
    }

    if (g_state.framebuffer) {
        return g_state.framebuffer;
    }

    /* Map framebuffer via mmap */
    g_state.framebuffer = mmap(NULL, g_state.fb_size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, g_state.fd, 0);

    if (g_state.framebuffer == MAP_FAILED) {
        g_state.framebuffer = NULL;
        return NULL;
    }

    return g_state.framebuffer;
}

void picasso_unmap_framebuffer(void)
{
    if (g_state.framebuffer) {
        munmap(g_state.framebuffer, g_state.fb_size);
        g_state.framebuffer = NULL;
    }
}

int picasso_get_framebuffer_size(uint16_t *width, uint16_t *height)
{
    if (!width || !height) {
        return PICASSO_ERROR_INVALID;
    }

    if (check_init() != PICASSO_SUCCESS) {
        return PICASSO_ERROR_INIT;
    }

    *width = g_state.width;
    *height = g_state.height;

    return PICASSO_SUCCESS;
}
