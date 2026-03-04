#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

extern int ioctl(int fd, unsigned long request, ...);
extern int tcgetattr(int fd, void *tio);
extern int posix_openpt(int flags);
extern int grantpt(int fd);
extern int unlockpt(int fd);
extern int ptsname_r(int fd, char *buf, size_t buflen);
extern DIR *fdopendir(int fd);
extern int dirfd(DIR *dirp);

static int g_failures = 0;

static const char *err_name(int err)
{
  switch(err){
  case 0:
    return "OK";
  case EINVAL:
    return "EINVAL";
  case ENOENT:
    return "ENOENT";
  case EBADF:
    return "EBADF";
  case EISDIR:
    return "EISDIR";
  case ENOTDIR:
    return "ENOTDIR";
  case ENOTTY:
    return "ENOTTY";
  case ERANGE:
    return "ERANGE";
  case ENOSYS:
    return "ENOSYS";
  case ESPIPE:
    return "ESPIPE";
  default:
    return "OTHER";
  }
}

static void probe(const char *name, int ok, int rc_norm, int err)
{
  printf("PROBE %s rc=%d errno=%s\n", name, rc_norm, err_name(err));
  if(!ok)
    g_failures++;
}

int main(void)
{
  int fd;
  int rc;
  int err;
  DIR *d;
  struct dirent *de;
  unsigned char tio[128];
  char pts_small[2];
  char pts_ok[32];
  int mfd;
  int dfd;
  struct timeval tv[2];

  errno = 0;
  fd = open("/no/such/path", O_RDONLY);
  err = errno;
  probe("open_missing", fd < 0 && err == ENOENT, (fd < 0) ? -1 : 0, err);
  if(fd >= 0)
    close(fd);

  errno = 0;
  fd = open("/", O_RDONLY);
  err = errno;
  probe("open_dir_rd", fd >= 0, (fd < 0) ? -1 : 0, (fd < 0) ? err : 0);
  if(fd >= 0)
    close(fd);

  errno = 0;
  fd = open("/", O_WRONLY);
  err = errno;
  probe("open_dir_wr", fd < 0 && err == EISDIR, (fd < 0) ? -1 : 0, err);
  if(fd >= 0)
    close(fd);

  errno = 0;
  fd = open("/etc/rc", O_RDONLY);
  if(fd < 0){
    probe("read_zero_null", 0, -1, errno);
  } else {
    errno = 0;
    rc = read(fd, 0, 0);
    err = errno;
    probe("read_zero_null", rc == 0 && err == 0, (rc < 0) ? -1 : 0, err);

    errno = 0;
    rc = write(fd, "x", 1);
    err = errno;
    probe("write_readonly", rc < 0 && err == EBADF, (rc < 0) ? -1 : 0, err);

    close(fd);
  }

  errno = 0;
  fd = open("/dev/tty", O_RDWR);
  if(fd < 0){
    probe("lseek_tty", 0, -1, errno);
  } else {
    errno = 0;
    rc = (int)lseek(fd, 0, SEEK_SET);
    err = errno;
    probe("lseek_tty", rc < 0 && err == ESPIPE, (rc < 0) ? -1 : 0, err);
    close(fd);
  }

  errno = 0;
  rc = fcntl(-1, F_GETFL);
  err = errno;
  probe("fcntl_badfd", rc < 0 && err == EBADF, (rc < 0) ? -1 : 0, err);

  errno = 0;
  fd = open("/etc/rc", O_RDONLY);
  if(fd < 0){
    probe("ioctl_notty", 0, -1, errno);
    probe("tcgetattr_file", 0, -1, errno);
  } else {
    errno = 0;
    rc = ioctl(fd, 0UL);
    err = errno;
    probe("ioctl_notty", rc < 0 && err == ENOTTY, (rc < 0) ? -1 : 0, err);

    errno = 0;
    rc = tcgetattr(fd, &tio);
    err = errno;
    probe("tcgetattr_file", rc < 0 && err == ENOTTY, (rc < 0) ? -1 : 0, err);
    close(fd);
  }

  errno = 0;
  fd = open("/dev/tty", O_RDWR);
  if(fd < 0){
    probe("tcgetattr_tty", 0, -1, errno);
  } else {
    errno = 0;
    rc = tcgetattr(fd, &tio);
    err = errno;
    probe("tcgetattr_tty", rc == 0 && err == 0, (rc < 0) ? -1 : 0, err);
    close(fd);
  }

  errno = 0;
  d = opendir("/no/such/dir");
  err = errno;
  probe("opendir_missing", d == 0 && err == ENOENT, (d == 0) ? -1 : 0, err);
  if(d)
    closedir(d);

  errno = 0;
  d = opendir("/etc/rc");
  err = errno;
  probe("opendir_file", d == 0 && err == ENOTDIR, (d == 0) ? -1 : 0, err);
  if(d)
    closedir(d);

  errno = 0;
  d = opendir("/");
  if(d == 0){
    probe("opendir_root", 0, -1, errno);
    probe("dirfd_root", 0, -1, errno);
    probe("readdir_root", 0, -1, errno);
  } else {
    probe("opendir_root", 1, 0, 0);
    errno = 0;
    dfd = dirfd(d);
    err = errno;
    probe("dirfd_root", dfd >= 0, (dfd < 0) ? -1 : 0, (dfd < 0) ? err : 0);

    errno = 0;
    de = readdir(d);
    err = errno;
    probe("readdir_root", de != 0, (de == 0) ? -1 : 0, (de == 0) ? err : 0);
    closedir(d);
  }

  errno = 0;
  mfd = posix_openpt(O_RDWR);
  if(mfd < 0){
    probe("posix_openpt_rdwr", 0, -1, errno);
    probe("grantpt", 0, -1, errno);
    probe("unlockpt", 0, -1, errno);
    probe("ptsname_r_small", 0, -1, errno);
    probe("ptsname_r_ok", 0, -1, errno);
  } else {
    probe("posix_openpt_rdwr", 1, 0, 0);

    errno = 0;
    rc = grantpt(mfd);
    err = errno;
    probe("grantpt", rc == 0 && err == 0, (rc < 0) ? -1 : 0, err);

    errno = 0;
    rc = unlockpt(mfd);
    err = errno;
    probe("unlockpt", rc == 0 && err == 0, (rc < 0) ? -1 : 0, err);

    errno = 0;
    rc = ptsname_r(mfd, pts_small, sizeof(pts_small));
    err = errno;
    probe("ptsname_r_small", rc == ERANGE && err == ERANGE, (rc != 0) ? -1 : 0, err);

    errno = 0;
    rc = ptsname_r(mfd, pts_ok, sizeof(pts_ok));
    err = errno;
    probe("ptsname_r_ok", rc == 0 && err == 0 && strncmp(pts_ok, "/dev/pts/", 9) == 0, (rc != 0) ? -1 : 0, err);
    close(mfd);
  }

  errno = 0;
  mfd = posix_openpt(O_RDONLY);
  err = errno;
  probe("posix_openpt_rdonly", mfd < 0 && err == EINVAL, (mfd < 0) ? -1 : 0, err);
  if(mfd >= 0)
    close(mfd);

  errno = 0;
  fd = open("/", O_RDONLY);
  if(fd < 0){
    probe("fdopendir", 0, -1, errno);
  } else {
    errno = 0;
    d = fdopendir(fd);
    err = errno;
    probe("fdopendir", d != 0 && err == 0, (d == 0) ? -1 : 0, (d == 0) ? err : 0);
    if(d)
      closedir(d);
    else
      close(fd);
  }

  fd = open("/tmp/probe_nosys", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if(fd >= 0)
    close(fd);

  fd = open("/tmp/probe_nosys", O_WRONLY | O_TRUNC);
  if(fd >= 0){
    (void)write(fd, "abc", 3);
    close(fd);
  }

  errno = 0;
  rc = truncate("/tmp/probe_nosys", 0);
  err = errno;
  if(rc == 0){
    struct stat st;
    if(stat("/tmp/probe_nosys", &st) != 0)
      rc = -1;
    else if(st.st_size != 0)
      rc = -1;
  }
  probe("truncate_zero", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

  memset(tv, 0, sizeof(tv));
  errno = 0;
  rc = utimes("/tmp/probe_nosys", tv);
  err = errno;
  probe("utimes_ok", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

  errno = 0;
  rc = (isspace(' ') && isdigit('7') && isxdigit('f') && !isspace('A') && !isdigit('x')) ? 0 : -1;
  err = errno;
  probe("ctype_ascii", rc == 0 && err == 0, rc, err);

  errno = 0;
  rc = chown("/tmp/probe_nosys", 0, 0);
  err = errno;
  probe("chown_ok", rc == 0 && err == 0, (rc < 0) ? -1 : 0, err);
  (void)unlink("/tmp/probe_nosys");

  printf("PROBE SUMMARY failures=%d\n", g_failures);
  return (g_failures == 0) ? 0 : 1;
}
