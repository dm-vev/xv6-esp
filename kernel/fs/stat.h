/**
 * @file stat.h
 * @brief File status structures and types
 *
 * File type constants:
 * - T_DIR: Directory
 * - T_FILE: Regular file
 * - T_DEVICE: Device file
 * - T_SYMLINK: Symbolic link
 *
 * Also defines struct stat for file metadata.
 */
#define T_DIR     1   // Directory
#define T_FILE    2   // File
#define T_DEVICE  3   // Device
#define T_SYMLINK 4   // Symbolic link

struct stat {
  int dev;     // File system's disk device
  uint ino;    // Inode number
  short type;  // Type of file
  short nlink; // Number of links to file
  uint64 size; // Size of file in bytes
};
