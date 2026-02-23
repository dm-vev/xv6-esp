/**
 * @file fcntl.h
 * @brief File control flags
 *
 * Open flags for file operations:
 * - O_RDONLY: Read only
 * - O_WRONLY: Write only
 * - O_RDWR: Read and write
 * - O_CREATE: Create if not exists
 * - O_TRUNC: Truncate to zero length
 */
#define O_RDONLY  0x000
#define O_WRONLY  0x001
#define O_RDWR    0x002
#define O_CREATE  0x200
#define O_TRUNC   0x400
