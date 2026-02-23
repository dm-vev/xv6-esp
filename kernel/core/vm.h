/**
 * @file vm.h
 * @brief Virtual memory configuration
 *
 * Memory allocation strategies:
 * - SBRK_EAGER: Allocate immediately
 * - SBRK_LAZY: Allocate on demand
 */
#define SBRK_EAGER 1
#define SBRK_LAZY  2
