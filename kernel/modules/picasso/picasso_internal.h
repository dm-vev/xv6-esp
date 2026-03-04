/**
 * @file picasso_internal.h
 * @brief Picasso Graphics Server - Internal Definitions
 */

#ifndef PICASSO_INTERNAL_H
#define PICASSO_INTERNAL_H

#include <stdlib.h>

/* Maximum number of registered backends */
#define PICASSO_MAX_BACKENDS 4

/* ============================================================================
 * PSRAM Configuration
 * ============================================================================
 *
 * PSRAM (Pseudo SRAM) provides additional memory beyond internal SRAM.
 * On ESP32-S3 (T-Deck), up to 8MB PSRAM is available.
 *
 * Trade-offs:
 * - PSRAM is slower than internal RAM but much larger
 * - DMA buffers MUST be in internal RAM
 * - Framebuffers can be in PSRAM for larger sizes
 * - Cache misses may cause slight latency
 */

/* Enable PSRAM support (set to 0 to disable, 1 to enable) */
#ifndef PICASSO_USE_PSRAM
#define PICASSO_USE_PSRAM 1
#endif

/* Minimum free internal RAM before using PSRAM (in bytes) */
#define PICASSO_PSRAM_THRESHOLD (64 * 1024)  /* 64 KB */

/* Force framebuffers to use PSRAM even if internal RAM is available */
#ifndef PICASSO_FORCE_PSRAM
#define PICASSO_FORCE_PSRAM 0
#endif

/* ============================================================================
 * Memory Allocation Helpers
 * ============================================================================ */

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_psram.h>

/* Capability flags */
#define MALLOC_CAP_DMA MALLOC_CAP_DMA
#define MALLOC_CAP_INTERNAL MALLOC_CAP_INTERNAL
#define MALLOC_CAP_SPIRAM MALLOC_CAP_SPIRAM

/* Wrapper for heap_caps_malloc */
#define heap_caps_malloc heap_caps_malloc

/* PSRAM availability check */
static inline int psram_available(void)
{
    return esp_psram_is_initialized() && (esp_psram_get_size() > 0);
}

/* Get PSRAM size */
static inline uint32_t psram_get_size(void)
{
    return esp_psram_get_size();
}

/* Get free PSRAM */
static inline uint32_t psram_get_free_size(void)
{
    return esp_psram_get_free_size();
}

/* Get free internal RAM */
static inline uint32_t internal_ram_get_free_size(void)
{
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

#else
/* Fallback for non-ESP builds - use standard malloc */
#define MALLOC_CAP_DMA 0
#define MALLOC_CAP_INTERNAL 0
#define MALLOC_CAP_SPIRAM 0
#define heap_caps_malloc(sz, caps) malloc(sz)

static inline int psram_available(void) { return 0; }
static inline uint32_t psram_get_size(void) { return 0; }
static inline uint32_t psram_get_free_size(void) { return 0; }
static inline uint32_t internal_ram_get_free_size(void) { return 0; }

#endif /* ESP_PLATFORM */

/* ============================================================================
 * Memory Allocation Functions
 * ============================================================================ */

/**
 * @brief Allocate framebuffer memory with PSRAM support
 *
 * Allocates memory for framebuffers, preferring internal RAM when available,
 * falling back to PSRAM for large allocations or when internal RAM is low.
 *
 * @param size Size in bytes to allocate
 * @param require_dma If true, forces internal RAM (required for DMA)
 * @return Pointer to allocated memory or NULL on failure
 */
static inline void* picasso_malloc_framebuffer(size_t size, int require_dma)
{
#ifdef ESP_PLATFORM
    if (require_dma) {
        /* DMA buffers must be in internal RAM */
        return heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }

    /* Check if PSRAM is available and enabled */
    if (PICASSO_USE_PSRAM && psram_available()) {
        /* Force PSRAM if configured */
        if (PICASSO_FORCE_PSRAM) {
            return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        }

        /* Check if internal RAM is getting low */
        uint32_t free_internal = internal_ram_get_free_size();
        if (free_internal < PICASSO_PSRAM_THRESHOLD) {
            /* Use PSRAM to preserve internal RAM */
            return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        }
    }

    /* Default: try internal RAM first */
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (ptr) {
        return ptr;
    }

    /* Fall back to PSRAM if available */
    if (PICASSO_USE_PSRAM && psram_available()) {
        ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
        if (ptr) {
            return ptr;
        }
    }

    return NULL;
#else
    return malloc(size);
#endif
}

/**
 * @brief Allocate DMA buffer (always in internal RAM)
 */
static inline void* picasso_malloc_dma(size_t size)
{
#ifdef ESP_PLATFORM
    return heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
#else
    return malloc(size);
#endif
}

/**
 * @brief Free memory allocated by picasso allocation functions
 */
static inline void picasso_free(void *ptr)
{
    free(ptr);
}

/**
 * @brief Get memory statistics
 */
typedef struct {
    uint32_t internal_total;
    uint32_t internal_free;
    uint32_t psram_total;
    uint32_t psram_free;
    uint32_t framebuffer_size;
    uint8_t using_psram;
} picasso_mem_stats_t;

/* Get current memory statistics (implementation in picasso.c) */
void picasso_get_memory_stats(picasso_mem_stats_t *stats);

#endif /* PICASSO_INTERNAL_H */
