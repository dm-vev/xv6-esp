#ifndef XV6_HOSTABI_DIRENT_H
#define XV6_HOSTABI_DIRENT_H

#include <dirent.h>

DIR *hostabi_opendir(const char *path);
struct dirent *hostabi_readdir(DIR *dirp);
int hostabi_closedir(DIR *dirp);
void hostabi_rewinddir(DIR *dirp);
int hostabi_dirfd(DIR *dirp);
DIR *hostabi_fdopendir(int fd);

#endif
