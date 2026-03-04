# Picasso Graphics Server

High-performance graphics server kernel module for xv6-esp with T-Deck backend support.

## Features

- **DMA Acceleration**: Zero-copy DMA transfers with ping-pong buffering
- **High Speed**: 80 MHz SPI clock for maximum throughput (~76 FPS theoretical)
- **Double Buffering**: Smooth animation support with front/back buffers
- **Thread Safety**: All operations protected by spinlocks
- **Modular Architecture**: Pluggable backend system for different displays
- **T-Deck Support**: Full support for LilyGo T-Deck hardware (320x240 ST7789)

## Architecture

```
┌─────────────────────────────────────┐
│        User Applications             │
│    (picasso_lib.c - user space)     │
├─────────────────────────────────────┤
│         Kernel Module                │
│  ┌──────────────────────────────┐  │
│  │     Picasso Core API         │  │
│  │  - Drawing primitives        │  │
│  │  - Buffer management         │  │
│  │  - Clipping & clipping       │  │
│  └──────────────────────────────┘  │
│  ┌──────────────────────────────┐  │
│  │      Backend Interface       │  │
│  │   (pluggable backends)       │  │
│  └──────────────────────────────┘  │
│           ↓                         │
│  ┌──────────────────────────────┐  │
│  │    T-Deck Backend            │  │
│  │  - ST7789 driver             │  │
│  │  - SPI @ 80 MHz              │  │
│  │  - DMA with ping-pong        │  │
│  │  - Backlight control         │  │
│  └──────────────────────────────┘  │
└─────────────────────────────────────┘
              ↓
┌─────────────────────────────────────┐
│         Hardware (T-Deck)          │
│  ┌─────────────────────────────┐  │
│  │   ST7789 320x240 LCD        │  │
│  │   SPI2 @ 80 MHz             │  │
│  │   DMA Channel Auto          │  │
│  │   BGR565 format             │  │
│  └─────────────────────────────┘  │
└─────────────────────────────────────┘
```

## File Structure

```
kernel/modules/picasso/
├── picasso.h              # Public API header
├── picasso.c              # Core implementation
├── picasso_internal.h     # Internal definitions
├── picasso_tdeck.c        # T-Deck backend
├── Makefile              # Build configuration
└── README.md             # This file

user/
├── picasso_lib.h         # User space library header
└── picasso_lib.c         # User space implementation
```

## Hardware Configuration (T-Deck)

- **Display**: ST7789 320x240 16-bit color LCD
- **SPI Host**: SPI2 @ 80 MHz (maximum ST7789 rated frequency)
- **DMA**: Automatic DMA transfer with ping-pong buffers (25600 bytes per buffer)
- **Performance**: ~25.6 MB/s raw throughput, theoretical 76 FPS @ 320x240

### Pinout

| Pin | GPIO | Function |
|-----|------|----------|
| MOSI | 41 | SPI data output |
| MISO | 38 | SPI data input |
| CLK | 40 | SPI clock (80 MHz) |
| CS | 12 | Chip select |
| DC | 11 | Data/Command |
| RST | NC | Reset (not connected) |
| BL | 42 | Backlight PWM |
| PWR | 10 | Display power enable |

## API Reference

### Core Functions

```c
// Initialization
int picasso_init(void);
void picasso_deinit(void);
int picasso_get_display_info(picasso_display_info_t *info);

// Display Control
int picasso_set_backlight(uint8_t level);  // 0-255
int picasso_clear(uint16_t color);
int picasso_flush(void);

// Drawing
int picasso_draw_pixel(int16_t x, int16_t y, uint16_t color);
int picasso_draw_line(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint16_t color);
int picasso_draw_rect(const picasso_rect_t *rect, uint16_t color);
int picasso_fill_rect(const picasso_rect_t *rect, uint16_t color);
int picasso_draw_circle(const picasso_point_t *center, uint16_t radius, uint16_t color);
int picasso_fill_circle(const picasso_point_t *center, uint16_t radius, uint16_t color);
int picasso_draw_bitmap(int16_t x, int16_t y, uint16_t w, uint16_t h, const uint16_t *data);
```

## Building

```bash
# From kernel/modules/picasso directory
cd kernel/modules/picasso
make

# This creates picasso.ko - the loadable kernel module
```

## Loading

```bash
# Load the module (from xv6 shell)
kmod load /lib/modules/picasso.ko

# Or auto-load from manifest
echo "/lib/modules/picasso.ko" > /etc/modules.manifest
kmod autoload /etc/modules.manifest
```

## Usage Example

```c
#include "picasso_lib.h"

int main() {
    // Initialize
    if (picasso_open() < 0) {
        printf("Failed to initialize Picasso\n");
        return 1;
    }

    // Clear screen to black
    picasso_clear(PICASSO_COLOR_BLACK);

    // Draw some shapes
    picasso_line(10, 10, 100, 100, PICASSO_COLOR_RED);

    picasso_rect_t rect = {50, 50, 100, 80};
    picasso_fill_rect(&rect, PICASSO_COLOR_BLUE);

    picasso_point_t center = {160, 120};
    picasso_fill_circle(&center, 30, PICASSO_COLOR_GREEN);

    // Flush to display
    picasso_flush();

    // Cleanup
    picasso_close();
    return 0;
}
```

## Performance Characteristics

### DMA Transfer

- **Ping-pong buffers**: 2x 25,600 bytes for zero-copy transfers
- **Chunked transfers**: Large bitmaps split for optimal DMA efficiency
- **Queue depth**: 4 SPI transactions for maximum pipelining
- **Bus saturation**: SPI bus utilization ~90% at 80 MHz

### Frame Rate Estimates

| Operation | Bandwidth | Theoretical FPS |
|-----------|-----------|-----------------|
| Full screen update | 25.6 MB/s | ~76 FPS |
| Partial update (1/4 screen) | 25.6 MB/s | ~304 FPS |
| Single line | 25.6 MB/s | ~24,000 FPS |

### SPI Configuration

- **Clock**: 80 MHz (ST7789 maximum rated frequency)
- **Mode**: Mode 0 (CPOL=0, CPHA=0)
- **Data order**: MSB first
- **Queue depth**: 4 transactions
- **Max transfer size**: 51,200 bytes (40 lines x 320 pixels x 2 bytes)

## PSRAM Support (Optional)

Picasso optionally supports PSRAM (Pseudo SRAM) for framebuffers on ESP32-S3 platforms.

### Memory Layout

| Memory Type | Purpose | Speed | Size |
|-------------|---------|-------|------|
| Internal RAM | DMA buffers, critical data | Fast | ~320 KB |
| PSRAM | Framebuffers, large buffers | Medium | Up to 8 MB |

### PSRAM Configuration

Configure at compile time in `picasso_internal.h`:

```c
/* Enable/disable PSRAM support */
#define PICASSO_USE_PSRAM 1

/* Minimum free internal RAM before using PSRAM (in bytes) */
#define PICASSO_PSRAM_THRESHOLD (64 * 1024)  /* 64 KB */

/* Force framebuffers to PSRAM even if internal RAM available */
#define PICASSO_FORCE_PSRAM 0
```

### PSRAM API

```c
/* Check PSRAM availability */
if (picasso_psram_available()) {
    printf("PSRAM: %u bytes total\n", picasso_psram_get_size());
    printf("PSRAM free: %u bytes\n", picasso_psram_get_free());
}

/* Check where framebuffers are stored */
if (picasso_using_psram()) {
    printf("Framebuffers are in PSRAM\n");
} else {
    printf("Framebuffers are in internal RAM\n");
}

/* Get memory statistics */
uint32_t internal_free, psram_free, fb_size;
int using_psram;
picasso_get_memory_info(&internal_free, &psram_free, &fb_size, &using_psram);
```

### Memory Strategy

- **DMA buffers** are always allocated in internal RAM (required by hardware)
- **Framebuffers** prefer internal RAM when available (faster access)
- Automatic fallback to PSRAM when internal RAM is below threshold
- Drawing operations work identically regardless of memory location

### Performance Considerations

- **PSRAM access** is ~3-4x slower than internal RAM due to cache misses
- **Frame rate impact** is minimal since drawing is CPU-bound, not memory-bound
- **DMA transfers** are not affected by framebuffer location
- Recommended for: Large framebuffers, multiple buffering, memory-constrained apps

## Safety & Performance

- **Thread Safety**: All operations protected by spinlocks with interrupt safety
- **Bounds Checking**: Automatic clipping to display bounds on all drawing operations
- **DMA Safety**: DMA buffers allocated in dedicated internal RAM (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)
- **Lock-free operations**: Drawing to framebuffer is lock-free, only flush acquires lock
- **Optimized Algorithms**: Bresenham line drawing, midpoint circle algorithm
- **Batch Operations**: Multiple pixels transferred in single SPI transaction
- **Memory alignment**: All buffers aligned for DMA efficiency

## License

Same as xv6-esp project.
