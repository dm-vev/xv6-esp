#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <time.h>
#include <unistd.h>

extern FILE *__xv6_host_fopen(const char *path, const char *mode);
extern int __xv6_host_fclose(FILE *f);
extern int __xv6_host_stat(const char *path, struct stat *st);
extern int __xv6_host_lstat(const char *path, struct stat *st);
extern int __xv6_host_fstat(int fd, struct stat *st);
extern DIR *__xv6_host_opendir(const char *path);
extern struct dirent *__xv6_host_readdir(DIR *d);
extern int __xv6_host_closedir(DIR *d);
extern void (*__xv6_host_signal(int sig, void (*handler)(int)))(int);
extern int __xv6_host_sigaction(int sig, const struct sigaction *act, struct sigaction *oldact);
extern int __xv6_host_gettimeofday(struct timeval *tp, void *tzp);
extern clock_t __xv6_host_times(struct tms *buf);
extern time_t __xv6_host_time(time_t *out);
extern int __xv6_host_getopt(int argc, char *const argv[], const char *optstring);

FILE *fopen(const char *path, const char *mode)
{
  return __xv6_host_fopen(path, mode);
}

int fclose(FILE *f)
{
  return __xv6_host_fclose(f);
}

int stat(const char *path, struct stat *st)
{
  return __xv6_host_stat(path, st);
}

int lstat(const char *path, struct stat *st)
{
  return __xv6_host_lstat(path, st);
}

int fstat(int fd, struct stat *st)
{
  return __xv6_host_fstat(fd, st);
}

DIR *opendir(const char *path)
{
  return __xv6_host_opendir(path);
}

struct dirent *readdir(DIR *d)
{
  return __xv6_host_readdir(d);
}

int closedir(DIR *d)
{
  return __xv6_host_closedir(d);
}

void (*signal(int sig, void (*handler)(int)))(int)
{
  return __xv6_host_signal(sig, handler);
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oldact)
{
  return __xv6_host_sigaction(sig, act, oldact);
}

int gettimeofday(struct timeval *tp, void *tzp)
{
  return __xv6_host_gettimeofday(tp, tzp);
}

clock_t times(struct tms *buf)
{
  return __xv6_host_times(buf);
}

time_t time(time_t *out)
{
  return __xv6_host_time(out);
}

int getopt(int argc, char *const argv[], const char *optstring)
{
  return __xv6_host_getopt(argc, argv, optstring);
}

int libc_so_anchor(void)
{
  return 0;
}
