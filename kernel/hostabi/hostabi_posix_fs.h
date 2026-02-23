#ifndef XV6_HOSTABI_POSIX_FS_H
#define XV6_HOSTABI_POSIX_FS_H

#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>

int hostabi_posix_fs_map_fd(int fd);
int hostabi_posix_fs_open_mode(const char *path, int flags, mode_t mode);
int hostabi_posix_fs_creat(const char *path, mode_t mode);
int hostabi_posix_fs_read(int fd, void *buf, size_t size);
int hostabi_posix_fs_write(int fd, const void *buf, size_t size);
int hostabi_posix_fs_close(int fd);
int hostabi_posix_fs_dup(int fd);
int hostabi_posix_fs_dup2(int oldfd, int newfd);
int hostabi_posix_fs_pipe(int pipefd[2]);
off_t hostabi_posix_fs_lseek(int fd, off_t offset, int whence);
int hostabi_posix_fs_fstat(int fd, struct stat *st);
int hostabi_posix_fs_stat(const char *path, struct stat *st);
int hostabi_posix_fs_lstat(const char *path, struct stat *st);
int hostabi_posix_fs_readlink(const char *path, char *buf, size_t bufsz);

#endif
