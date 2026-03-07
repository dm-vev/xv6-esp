#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "xv6_termios_compat.h"
#include "xv6_socket_compat.h"

extern int ioctl(int fd, unsigned long request, ...);
extern int tcgetattr(int fd, void *tio);
extern int tcsetattr(int fd, int optional_actions, const void *tio);
extern int posix_openpt(int flags);
extern int grantpt(int fd);
extern int unlockpt(int fd);
extern int ptsname_r(int fd, char *buf, size_t buflen);
extern DIR *fdopendir(int fd);
extern int dirfd(DIR *dirp);
extern int shrt_eval_line(const char *line, int *exit_code);
extern int waitid(int idtype, int id, void *infop, int options);
extern pid_t getpgid(pid_t pid);
extern pid_t getpgrp(void);
extern pid_t getsid(pid_t pid);
extern int setpgid(pid_t pid, pid_t pgid);
extern pid_t setsid(void);
extern pid_t tcgetpgrp(int fd);
extern int tcsetpgrp(int fd, pid_t pgrp);
extern int openat(int dirfd, const char *path, int flags, ...);
extern int fstatat(int dirfd, const char *path, struct stat *st, int flags);
extern int unlinkat(int dirfd, const char *path, int flags);
extern int renameat(int olddirfd, const char *oldpath, int newdirfd, const char *newpath);
extern int mkdirat(int dirfd, const char *path, mode_t mode);
extern int symlinkat(const char *target, int newdirfd, const char *linkpath);
extern ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsz);
extern int sigsuspend(const sigset_t *mask);

#ifndef WEXITED
#define WEXITED 4
#endif
#ifndef WSTOPPED
#define WSTOPPED 2
#endif
#ifndef WNOWAIT
#define WNOWAIT 0x01000000
#endif
#ifndef P_ALL
#define P_ALL 0
#endif
#ifndef CLD_EXITED
#define CLD_EXITED 1
#endif
#ifndef CLD_STOPPED
#define CLD_STOPPED 5
#endif
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif
#ifndef AT_SYMLINK_NOFOLLOW
#define AT_SYMLINK_NOFOLLOW 0x100
#endif
#ifndef AT_REMOVEDIR
#define AT_REMOVEDIR 0x200
#endif

static int g_failures = 0;
static volatile sig_atomic_t g_sigusr1_hits = 0;

static void probe_sigusr1_handler(int sig)
{
  (void)sig;
  g_sigusr1_hits++;
}

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
  case EPERM:
    return "EPERM";
  case ESRCH:
    return "ESRCH";
  case ERANGE:
    return "ERANGE";
  case ENOSYS:
    return "ENOSYS";
  case ECHILD:
    return "ECHILD";
  case EAGAIN:
    return "EAGAIN";
  case EINTR:
    return "EINTR";
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

static int read_small_file(const char *path, char *out, size_t out_sz)
{
  FILE *f;
  size_t n;

  if(path == 0 || out == 0 || out_sz < 2)
    return -1;
  f = fopen(path, "r");
  if(f == 0)
    return -1;
  n = fread(out, 1, out_sz - 1, f);
  out[n] = 0;
  fclose(f);
  return 0;
}

static int wait_for_stop_event(siginfo_t *si, int *out_pid, int *out_sig)
{
  int i;

  if(si == 0 || out_pid == 0 || out_sig == 0)
    return -1;
  *out_pid = -1;
  *out_sig = -1;

  for(i = 0; i < 200; i++){
    unsigned int packed_si = 0;

    memset(si, 0, sizeof(*si));
    errno = 0;
    if(waitid(P_ALL, 0, si, WSTOPPED | WNOHANG | WNOWAIT) < 0)
      return -1;
    if(si->si_signo == 0){
      usleep(10000);
      continue;
    }

    packed_si = (unsigned int)si->si_value.sival_int;
    *out_pid = (int)((packed_si >> 16) & 0xffffu);
    *out_sig = (int)(packed_si & 0xffffu);
    return 0;
  }

  errno = EAGAIN;
  return -1;
}

static int wait_for_exit_event(siginfo_t *si, int *out_pid, int *out_status)
{
  int i;

  if(si == 0 || out_pid == 0 || out_status == 0)
    return -1;
  *out_pid = -1;
  *out_status = -1;

  for(i = 0; i < 400; i++){
    unsigned int packed_si = 0;

    memset(si, 0, sizeof(*si));
    errno = 0;
    if(waitid(P_ALL, 0, si, WEXITED | WNOHANG | WNOWAIT) < 0)
      return -1;
    if(si->si_signo == 0){
      usleep(10000);
      continue;
    }

    packed_si = (unsigned int)si->si_value.sival_int;
    *out_pid = (int)((packed_si >> 16) & 0xffffu);
    *out_status = (int)(packed_si & 0xffffu);
    return 0;
  }

  errno = EAGAIN;
  return -1;
}

static int parse_first_job_id(const char *text)
{
  int id = 0;

  if(text == 0)
    return -1;
  while(*text){
    if(*text == '['){
      text++;
      if(*text < '0' || *text > '9')
        continue;
      while(*text >= '0' && *text <= '9'){
        id = (id * 10) + (*text - '0');
        text++;
      }
      return (id > 0) ? id : -1;
    }
    text++;
  }
  return -1;
}

int main(void)
{
  int fd;
  int rc;
  int err;
  DIR *d;
  struct dirent *de;
  struct termios tio;
  struct termios tio_saved;
  struct termios tio_peer;
  struct termios tio_check;
  char pts_small[2];
  char pts_ok[32];
  int mfd;
  int sfd;
  int dfd;
  struct timeval tv[2];
  union {
    siginfo_t si;
    unsigned char raw[sizeof(siginfo_t)];
  } si_buf;
  pid_t pid;
  int status;
  int shell_rc;
  int shell_exit = 0;
  int waitid_siginfo_pid = -1;
  int waitid_siginfo_status = -1;
  int waitid_have_siginfo = 0;
  struct sigaction sa;
  sigset_t sigset_tmp;
  sigset_t sigset_old;
  sigset_t sigset_cur;
  int sig_member = 0;
  struct timeval sel_tv;
  pid_t pgid = -1;
  pid_t sid = -1;
  struct stat at_st;
  char at_link[64];
  int at_dfd = -1;
  int at_fd = -1;
  char file_buf[256];
  char tty_buf[8];
  int stop_pid = -1;
  int stop_sig = -1;

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

  (void)unlink("/tmp/probe_dup2_target");
  errno = 0;
  fd = open("/etc/rc", O_RDONLY);
  if(fd < 0){
    probe("fcntl_setfd_cloexec", 0, -1, errno);
    probe("dup_clears_cloexec", 0, -1, errno);
    probe("fcntl_dupfd_clears_cloexec", 0, -1, errno);
#ifdef F_DUPFD_CLOEXEC
    probe("fcntl_dupfd_cloexec_sets", 0, -1, errno);
#endif
    probe("fcntl_setfl_nonblock", 0, -1, errno);
    probe("fcntl_getfl_nonblock", 0, -1, errno);
    probe("dup2_clears_cloexec", 0, -1, errno);
  } else {
    int dupfd = -1;
    int dupfd_min = -1;
    int dupfd_ce = -1;
    int target_fd = -1;
    int fl;

    errno = 0;
    rc = fcntl(fd, F_SETFD, FD_CLOEXEC);
    err = errno;
    probe("fcntl_setfd_cloexec", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

    errno = 0;
    dupfd = dup(fd);
    err = errno;
    if(dupfd < 0){
      probe("dup_clears_cloexec", 0, -1, err);
    } else {
      errno = 0;
      fl = fcntl(dupfd, F_GETFD);
      err = errno;
      probe("dup_clears_cloexec", fl == 0 && err == 0, (fl < 0) ? -1 : 0, (fl < 0) ? err : 0);
      close(dupfd);
    }

    errno = 0;
    dupfd_min = fcntl(fd, F_DUPFD, 3);
    err = errno;
    if(dupfd_min < 0){
      probe("fcntl_dupfd_clears_cloexec", 0, -1, err);
    } else {
      errno = 0;
      fl = fcntl(dupfd_min, F_GETFD);
      err = errno;
      probe("fcntl_dupfd_clears_cloexec", fl == 0 && err == 0, (fl < 0) ? -1 : 0, (fl < 0) ? err : 0);
      close(dupfd_min);
    }

#ifdef F_DUPFD_CLOEXEC
    errno = 0;
    dupfd_ce = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    err = errno;
    if(dupfd_ce < 0){
      probe("fcntl_dupfd_cloexec_sets", 0, -1, err);
    } else {
      errno = 0;
      fl = fcntl(dupfd_ce, F_GETFD);
      err = errno;
      probe("fcntl_dupfd_cloexec_sets", fl == FD_CLOEXEC && err == 0, (fl < 0) ? -1 : 0, (fl < 0) ? err : 0);
      close(dupfd_ce);
    }
#endif

    errno = 0;
    rc = fcntl(fd, F_SETFL, O_NONBLOCK);
    err = errno;
    probe("fcntl_setfl_nonblock", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

    errno = 0;
    fl = fcntl(fd, F_GETFL);
    err = errno;
    probe("fcntl_getfl_nonblock", fl >= 0 && (fl & O_NONBLOCK) != 0 && err == 0, (fl < 0) ? -1 : 0, (fl < 0) ? err : 0);

    errno = 0;
    target_fd = open("/tmp/probe_dup2_target", O_CREAT | O_RDWR | O_TRUNC, 0644);
    err = errno;
    if(target_fd < 0){
      probe("dup2_clears_cloexec", 0, -1, err);
    } else {
      errno = 0;
      rc = dup2(fd, target_fd);
      err = errno;
      if(rc < 0){
        probe("dup2_clears_cloexec", 0, -1, err);
      } else {
        errno = 0;
        fl = fcntl(target_fd, F_GETFD);
        err = errno;
        probe("dup2_clears_cloexec", fl == 0 && err == 0, (fl < 0) ? -1 : 0, (fl < 0) ? err : 0);
      }
      close(target_fd);
    }

    close(fd);
  }
  (void)unlink("/tmp/probe_dup2_target");

  errno = 0;
  fd = open("/etc/rc", O_RDONLY);
  if(fd < 0){
    probe("ioctl_notty", 0, -1, errno);
    probe("tcgetattr_file", 0, -1, errno);
    probe("isatty_devnull", 0, -1, errno);
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

    errno = 0;
    fd = open("/dev/null", O_RDONLY);
    err = errno;
    if(fd < 0){
      probe("isatty_devnull", 0, -1, err);
    } else {
      errno = 0;
      rc = isatty(fd);
      err = errno;
      probe("isatty_devnull", rc == 0 && err == ENOTTY, (rc != 0) ? 0 : -1, err);
      close(fd);
    }
  }

  errno = 0;
  fd = open("/dev/tty", O_RDWR);
  if(fd < 0){
    probe("tcgetattr_tty", 0, -1, errno);
    probe("tty_tcsetattr_shared", 0, -1, errno);
  } else {
    errno = 0;
    rc = tcgetattr(fd, &tio);
    err = errno;
    probe("tcgetattr_tty", rc == 0 && err == 0, (rc < 0) ? -1 : 0, err);

    errno = 0;
    rc = tcgetattr(0, &tio_saved);
    err = errno;
    if(rc == 0 && err == 0){
      tio_peer = tio_saved;
#ifdef ECHO
      tio_peer.c_lflag ^= ECHO;
#else
      tio_peer.c_cc[VMIN] = (cc_t)((tio_peer.c_cc[VMIN] == 1) ? 2 : 1);
#endif
      errno = 0;
      rc = tcsetattr(fd, TCSANOW, &tio_peer);
      err = errno;
      if(rc == 0 && err == 0){
        errno = 0;
        rc = tcgetattr(0, &tio_check);
        err = errno;
        probe("tty_tcsetattr_shared",
              rc == 0 && err == 0 &&
#ifdef ECHO
                  ((tio_check.c_lflag & ECHO) == (tio_peer.c_lflag & ECHO)),
#else
                  (tio_check.c_cc[VMIN] == tio_peer.c_cc[VMIN]),
#endif
              (rc < 0) ? -1 : 0,
              (rc < 0) ? err : 0);
      } else {
        probe("tty_tcsetattr_shared", 0, -1, err);
      }
      (void)tcsetattr(0, TCSANOW, &tio_saved);
    } else {
      probe("tty_tcsetattr_shared", 0, -1, err);
    }
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
    probe("pty_tcgetattr_master", 0, -1, errno);
    probe("pty_tcsetattr_shared", 0, -1, errno);
    probe("pty_tcsetattr_flush", 0, -1, errno);
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

    sfd = open(pts_ok, O_RDWR);
    if(sfd < 0){
      probe("pty_tcgetattr_master", 0, -1, errno);
      probe("pty_tcsetattr_shared", 0, -1, errno);
      probe("pty_tcsetattr_flush", 0, -1, errno);
    } else {
      errno = 0;
      rc = tcgetattr(mfd, &tio);
      err = errno;
      probe("pty_tcgetattr_master", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

      errno = 0;
      rc = tcgetattr(sfd, &tio_saved);
      err = errno;
      if(rc == 0 && err == 0){
        tio_peer = tio;
#ifdef ECHO
        tio_peer.c_lflag ^= ECHO;
#else
        tio_peer.c_cc[VMIN] = (cc_t)((tio_peer.c_cc[VMIN] == 1) ? 2 : 1);
#endif
        errno = 0;
        rc = tcsetattr(mfd, TCSANOW, &tio_peer);
        err = errno;
        if(rc == 0 && err == 0){
          errno = 0;
          rc = tcgetattr(sfd, &tio_check);
          err = errno;
          probe("pty_tcsetattr_shared",
                rc == 0 && err == 0 &&
#ifdef ECHO
                    ((tio_check.c_lflag & ECHO) == (tio_peer.c_lflag & ECHO)),
#else
                    (tio_check.c_cc[VMIN] == tio_peer.c_cc[VMIN]),
#endif
                (rc < 0) ? -1 : 0,
                (rc < 0) ? err : 0);
          (void)tcsetattr(mfd, TCSANOW, &tio);
        } else {
          probe("pty_tcsetattr_shared", 0, -1, err);
        }
      } else {
        probe("pty_tcsetattr_shared", 0, -1, err);
      }

      errno = 0;
      if(rc == 0 && err == 0 && write(mfd, "q", 1) == 1 && read(sfd, tty_buf, 1) == 1 && tty_buf[0] == 'q' &&
         write(mfd, "z", 1) == 1)
      {
        errno = 0;
        rc = tcsetattr(sfd, TCSAFLUSH, &tio_saved);
        err = errno;
        if(rc == 0 && err == 0){
          errno = 0;
          rc = read(sfd, tty_buf, 1);
          err = errno;
          probe("pty_tcsetattr_flush", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);
        } else {
          probe("pty_tcsetattr_flush", 0, -1, err);
        }
      } else {
        probe("pty_tcsetattr_flush", 0, -1, err);
      }

      if(write(mfd, "r", 1) == 1){
        struct pollfd pfd;
        fd_set rfds_local;

        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = sfd;
        pfd.events = POLLIN;
        errno = 0;
        rc = poll(&pfd, 1, 0);
        err = errno;
        probe("poll_pty_slave_read_ready",
              rc == 1 && err == 0 && (pfd.revents & POLLIN) != 0,
              (rc < 0) ? -1 : rc,
              (rc < 0) ? err : 0);

        FD_ZERO(&rfds_local);
        FD_SET(sfd, &rfds_local);
        sel_tv.tv_sec = 0;
        sel_tv.tv_usec = 0;
        errno = 0;
        rc = select(sfd + 1, &rfds_local, 0, 0, &sel_tv);
        err = errno;
        probe("select_pty_slave_read_ready",
              rc == 1 && err == 0 && FD_ISSET(sfd, &rfds_local),
              (rc < 0) ? -1 : rc,
              (rc < 0) ? err : 0);
        (void)read(sfd, tty_buf, 1);
      } else {
        probe("poll_pty_slave_read_ready", 0, -1, errno);
        probe("select_pty_slave_read_ready", 0, -1, errno);
      }
      close(sfd);
      sfd = -1;
    }
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

  (void)unlink("/tmp/probe_at_dir/file2");
  (void)unlink("/tmp/probe_at_dir/file");
  (void)unlink("/tmp/probe_at_dir/link");
  (void)unlink("/tmp/probe_at_dir/sub/.keep");
  (void)rmdir("/tmp/probe_at_dir/sub");
  (void)rmdir("/tmp/probe_at_dir");
  (void)mkdir("/tmp/probe_at_dir", 0755);

  errno = 0;
  at_dfd = open("/tmp/probe_at_dir", O_RDONLY);
  if(at_dfd < 0){
    probe("at_openat_rel", 0, -1, errno);
    probe("at_fstatat_reg", 0, -1, errno);
    probe("at_symlinkat", 0, -1, errno);
    probe("at_readlinkat", 0, -1, errno);
    probe("at_fstatat_nofollow", 0, -1, errno);
    probe("at_renameat", 0, -1, errno);
    probe("at_mkdirat", 0, -1, errno);
    probe("at_unlinkat_file", 0, -1, errno);
    probe("at_unlinkat_dir", 0, -1, errno);
  } else {
    errno = 0;
    at_fd = openat(at_dfd, "file", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    err = errno;
    if(at_fd >= 0){
      (void)write(at_fd, "ok", 2);
      close(at_fd);
    }
    probe("at_openat_rel", at_fd >= 0 && err == 0, (at_fd < 0) ? -1 : 0, (at_fd < 0) ? err : 0);

    memset(&at_st, 0, sizeof(at_st));
    errno = 0;
    rc = fstatat(at_dfd, "file", &at_st, 0);
    err = errno;
    probe("at_fstatat_reg", rc == 0 && err == 0 && S_ISREG(at_st.st_mode), (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

    errno = 0;
    rc = symlinkat("file", at_dfd, "link");
    err = errno;
    probe("at_symlinkat", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

    memset(at_link, 0, sizeof(at_link));
    errno = 0;
    rc = (int)readlinkat(at_dfd, "link", at_link, sizeof(at_link) - 1);
    err = errno;
    if(rc >= 0 && rc < (int)sizeof(at_link))
      at_link[rc] = 0;
    probe("at_readlinkat", rc > 0 && err == 0 && strcmp(at_link, "file") == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

    memset(&at_st, 0, sizeof(at_st));
    errno = 0;
    rc = fstatat(at_dfd, "link", &at_st, AT_SYMLINK_NOFOLLOW);
    err = errno;
    probe("at_fstatat_nofollow",
          rc == 0 && err == 0 && S_ISLNK(at_st.st_mode),
          (rc < 0) ? -1 : 0,
          (rc < 0) ? err : 0);

    errno = 0;
    rc = renameat(at_dfd, "file", at_dfd, "file2");
    err = errno;
    probe("at_renameat", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

    errno = 0;
    rc = mkdirat(at_dfd, "sub", 0755);
    err = errno;
    probe("at_mkdirat", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

    errno = 0;
    rc = unlinkat(at_dfd, "file2", 0);
    err = errno;
    probe("at_unlinkat_file", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

    errno = 0;
    rc = unlinkat(at_dfd, "sub", AT_REMOVEDIR);
    err = errno;
    probe("at_unlinkat_dir", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

    (void)unlinkat(at_dfd, "link", 0);
    close(at_dfd);
  }
  (void)rmdir("/tmp/probe_at_dir");

  errno = 0;
  at_fd = open("/etc/rc", O_RDONLY);
  if(at_fd < 0){
    probe("at_openat_nondirfd", 0, -1, errno);
  } else {
    errno = 0;
    rc = openat(at_fd, "x", O_RDONLY);
    err = errno;
    probe("at_openat_nondirfd", rc < 0 && err == ENOTDIR, (rc < 0) ? -1 : 0, err);
    close(at_fd);
  }

  (void)unlink("/tmp/probe_nosys");
  fd = creat("/tmp/probe_nosys", 0644);
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

  errno = 0;
  rc = poll(0, 0, -2);
  err = errno;
  probe("poll_bad_timeout", rc < 0 && err == EINVAL, (rc < 0) ? -1 : 0, err);

  errno = 0;
  rc = poll(0, 0, 0);
  err = errno;
  probe("poll_zero_ok", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

  {
    int pipefd[2];
    struct pollfd pfd;
    fd_set rfds_local;

    errno = 0;
    rc = pipe(pipefd);
    err = errno;
    if(rc != 0){
      probe("poll_pipe_write_ready", 0, -1, err);
      probe("poll_pipe_read_ready", 0, -1, err);
      probe("poll_pipe_hup", 0, -1, err);
      probe("select_pipe_read_hup", 0, -1, err);
    } else {
      memset(&pfd, 0, sizeof(pfd));
      pfd.fd = pipefd[1];
      pfd.events = POLLOUT;
      errno = 0;
      rc = poll(&pfd, 1, 0);
      err = errno;
      probe("poll_pipe_write_ready",
            rc == 1 && err == 0 && (pfd.revents & POLLOUT) != 0,
            (rc < 0) ? -1 : rc,
            (rc < 0) ? err : 0);

      (void)write(pipefd[1], "p", 1);
      memset(&pfd, 0, sizeof(pfd));
      pfd.fd = pipefd[0];
      pfd.events = POLLIN;
      errno = 0;
      rc = poll(&pfd, 1, 0);
      err = errno;
      probe("poll_pipe_read_ready",
            rc == 1 && err == 0 && (pfd.revents & POLLIN) != 0,
            (rc < 0) ? -1 : rc,
            (rc < 0) ? err : 0);

      (void)read(pipefd[0], tty_buf, 1);
      close(pipefd[1]);
      memset(&pfd, 0, sizeof(pfd));
      pfd.fd = pipefd[0];
      pfd.events = POLLIN;
      errno = 0;
      rc = poll(&pfd, 1, 0);
      err = errno;
      probe("poll_pipe_hup",
            rc == 1 && err == 0 && (pfd.revents & POLLHUP) != 0,
            (rc < 0) ? -1 : rc,
            (rc < 0) ? err : 0);

      FD_ZERO(&rfds_local);
      FD_SET(pipefd[0], &rfds_local);
      sel_tv.tv_sec = 0;
      sel_tv.tv_usec = 0;
      errno = 0;
      rc = select(pipefd[0] + 1, &rfds_local, 0, 0, &sel_tv);
      err = errno;
      probe("select_pipe_read_hup",
            rc == 1 && err == 0 && FD_ISSET(pipefd[0], &rfds_local),
            (rc < 0) ? -1 : rc,
            (rc < 0) ? err : 0);

      close(pipefd[0]);
    }
  }

  {
    int fifo_fd = -1;
    struct pollfd pfd;
    fd_set rfds_local;

    (void)unlink("/tmp/probe_fifo");
    errno = 0;
    rc = mkfifo("/tmp/probe_fifo", 0644);
    err = errno;
    if(rc != 0){
      probe("poll_fifo_write_ready", 0, -1, err);
      probe("select_fifo_read_ready", 0, -1, err);
    } else {
      errno = 0;
      fifo_fd = open("/tmp/probe_fifo", O_RDWR);
      err = errno;
      if(fifo_fd < 0){
        probe("poll_fifo_write_ready", 0, -1, err);
        probe("select_fifo_read_ready", 0, -1, err);
      } else {
        memset(&pfd, 0, sizeof(pfd));
        pfd.fd = fifo_fd;
        pfd.events = POLLOUT;
        errno = 0;
        rc = poll(&pfd, 1, 0);
        err = errno;
        probe("poll_fifo_write_ready",
              rc == 1 && err == 0 && (pfd.revents & POLLOUT) != 0,
              (rc < 0) ? -1 : rc,
              (rc < 0) ? err : 0);

        (void)write(fifo_fd, "f", 1);
        FD_ZERO(&rfds_local);
        FD_SET(fifo_fd, &rfds_local);
        sel_tv.tv_sec = 0;
        sel_tv.tv_usec = 0;
        errno = 0;
        rc = select(fifo_fd + 1, &rfds_local, 0, 0, &sel_tv);
        err = errno;
        probe("select_fifo_read_ready",
              rc == 1 && err == 0 && FD_ISSET(fifo_fd, &rfds_local),
              (rc < 0) ? -1 : rc,
              (rc < 0) ? err : 0);
        (void)read(fifo_fd, tty_buf, 1);
        close(fifo_fd);
      }
    }
    (void)unlink("/tmp/probe_fifo");
  }

  sel_tv.tv_sec = 0;
  sel_tv.tv_usec = 0;
  errno = 0;
  rc = select(0, 0, 0, 0, &sel_tv);
  err = errno;
  probe("select_zero_ok", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

  sel_tv.tv_sec = 0;
  sel_tv.tv_usec = 1000000;
  errno = 0;
  rc = select(0, 0, 0, 0, &sel_tv);
  err = errno;
  probe("select_bad_timeval", rc < 0 && err == EINVAL, (rc < 0) ? -1 : 0, err);

  (void)unlink("/tmp/probe_and_skip");
  (void)unlink("/tmp/probe_or_ok");
  (void)unlink("/tmp/probe_seq_a");
  (void)unlink("/tmp/probe_seq_b");
  (void)unlink("/tmp/probe_var");
  (void)unlink("/tmp/probe_status");
  (void)unlink("/tmp/probe_numredir");
  (void)unlink("/tmp/probe_g_a");
  (void)unlink("/tmp/probe_g_b");
  (void)unlink("/tmp/probe_glob_q");
  (void)unlink("/tmp/probe_glob_cls");

  errno = 0;
  shell_rc = shrt_eval_line("cat /no_such_file && echo and-ok >/tmp/probe_and_skip", &shell_exit);
  err = errno;
  probe("shell_and_shortcircuit",
        shell_rc == 0 && shell_exit != 0 && access("/tmp/probe_and_skip", F_OK) != 0,
        (shell_rc < 0) ? -1 : 0,
        (shell_rc < 0) ? err : 0);

  errno = 0;
  shell_rc = shrt_eval_line("cat /no_such_file || echo or-ok >/tmp/probe_or_ok", &shell_exit);
  err = errno;
  probe("shell_or_shortcircuit",
        shell_rc == 0 && shell_exit == 0 && access("/tmp/probe_or_ok", F_OK) == 0,
        (shell_rc < 0) ? -1 : 0,
        (shell_rc < 0) ? err : 0);

  errno = 0;
  shell_rc = shrt_eval_line("echo one >/tmp/probe_seq_a; echo two >/tmp/probe_seq_b", &shell_exit);
  err = errno;
  probe("shell_seq_semicolon",
        shell_rc == 0 && shell_exit == 0 && access("/tmp/probe_seq_a", F_OK) == 0 && access("/tmp/probe_seq_b", F_OK) == 0,
        (shell_rc < 0) ? -1 : 0,
        (shell_rc < 0) ? err : 0);

  errno = 0;
  shell_rc = shrt_eval_line("export PROBEVAR=zxv", &shell_exit);
  err = errno;
  if(shell_rc == 0 && shell_exit == 0)
    shell_rc = shrt_eval_line("echo $PROBEVAR >/tmp/probe_var", &shell_exit);
  if(shell_rc == 0 && shell_exit == 0){
    int ok = (read_small_file("/tmp/probe_var", file_buf, sizeof(file_buf)) == 0 && strstr(file_buf, "zxv") != 0);
    probe("shell_var_expand", ok, ok ? 0 : -1, 0);
  } else {
    probe("shell_var_expand", 0, -1, (shell_rc < 0) ? err : 0);
  }

  errno = 0;
  shell_rc = shrt_eval_line("cat /no_such_file; echo $? >/tmp/probe_status", &shell_exit);
  err = errno;
  if(shell_rc == 0 && shell_exit == 0){
    int ok = (read_small_file("/tmp/probe_status", file_buf, sizeof(file_buf)) == 0 && file_buf[0] >= '1' &&
              file_buf[0] <= '9');
    probe("shell_status_expand", ok, ok ? 0 : -1, 0);
  } else {
    probe("shell_status_expand", 0, -1, (shell_rc < 0) ? err : 0);
  }

  errno = 0;
  shell_rc = shrt_eval_line("echo 1 >/tmp/probe_numredir", &shell_exit);
  err = errno;
  if(shell_rc == 0 && shell_exit == 0){
    int ok = (read_small_file("/tmp/probe_numredir", file_buf, sizeof(file_buf)) == 0 && file_buf[0] == '1');
    probe("shell_numarg_redir", ok, ok ? 0 : -1, 0);
  } else {
    probe("shell_numarg_redir", 0, -1, (shell_rc < 0) ? err : 0);
  }

  errno = 0;
  shell_rc = shrt_eval_line("echo ga >/tmp/probe_g_a; echo gb >/tmp/probe_g_b", &shell_exit);
  err = errno;
  if(shell_rc == 0 && shell_exit == 0)
    shell_rc = shrt_eval_line("echo /tmp/probe_g_? >/tmp/probe_glob_q", &shell_exit);
  if(shell_rc == 0 && shell_exit == 0){
    int ok = (read_small_file("/tmp/probe_glob_q", file_buf, sizeof(file_buf)) == 0 &&
              strstr(file_buf, "/tmp/probe_g_a") != 0 && strstr(file_buf, "/tmp/probe_g_b") != 0);
    probe("shell_glob_qmark", ok, ok ? 0 : -1, 0);
  } else {
    probe("shell_glob_qmark", 0, -1, (shell_rc < 0) ? err : 0);
  }

  errno = 0;
  shell_rc = shrt_eval_line("echo /tmp/probe_g_[ab] >/tmp/probe_glob_cls", &shell_exit);
  err = errno;
  if(shell_rc == 0 && shell_exit == 0){
    int ok = (read_small_file("/tmp/probe_glob_cls", file_buf, sizeof(file_buf)) == 0 &&
              strstr(file_buf, "/tmp/probe_g_a") != 0 && strstr(file_buf, "/tmp/probe_g_b") != 0);
    probe("shell_glob_class", ok, ok ? 0 : -1, 0);
  } else {
    probe("shell_glob_class", 0, -1, (shell_rc < 0) ? err : 0);
  }

  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = probe_sigusr1_handler;
  (void)sigemptyset(&sa.sa_mask);
  g_sigusr1_hits = 0;
  errno = 0;
  rc = sigaction(SIGUSR1, &sa, 0);
  err = errno;
  probe("sigaction_usr1_set", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

  (void)sigemptyset(&sigset_tmp);
  (void)sigaddset(&sigset_tmp, SIGUSR1);
  errno = 0;
  rc = sigprocmask(SIG_BLOCK, &sigset_tmp, &sigset_old);
  err = errno;
  probe("sigprocmask_block_usr1", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

  errno = 0;
  rc = raise(SIGUSR1);
  err = errno;
  probe("raise_usr1_pending", rc == 0 && err == 0 && g_sigusr1_hits == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

  (void)sigemptyset(&sigset_tmp);
  errno = 0;
  rc = sigsuspend(&sigset_tmp);
  err = errno;
  probe("sigsuspend_eintr", rc < 0 && err == EINTR && g_sigusr1_hits == 1, (rc < 0) ? -1 : 0, err);

  errno = 0;
  rc = sigprocmask(SIG_SETMASK, 0, &sigset_cur);
  err = errno;
  if(rc == 0 && err == 0)
    sig_member = sigismember(&sigset_cur, SIGUSR1);
  else
    sig_member = -1;
  probe("sigsuspend_mask_restore",
        rc == 0 && err == 0 && sig_member == 1,
        (rc < 0) ? -1 : ((sig_member == 1) ? 0 : -1),
        (rc < 0) ? err : 0);

  (void)sigprocmask(SIG_SETMASK, &sigset_old, 0);

  errno = 0;
  pid = waitpid((pid_t)-1, &status, WNOHANG);
  err = errno;
  probe("waitpid_nochild", pid < 0 && err == ECHILD, (pid < 0) ? -1 : 0, err);

  memset(&si_buf, 0, sizeof(si_buf));
  errno = 0;
  rc = waitid(P_ALL, 0, si_buf.raw, 0);
  err = errno;
  probe("waitid_badopts", rc < 0 && err == EINVAL, (rc < 0) ? -1 : 0, err);

  memset(&si_buf, 0, sizeof(si_buf));
  errno = 0;
  rc = waitid(99, 0, si_buf.raw, WEXITED | WNOHANG);
  err = errno;
  probe("waitid_badidtype", rc < 0 && err == EINVAL, (rc < 0) ? -1 : 0, err);

  errno = 0;
  shell_rc = shrt_eval_line("sleep 2000 &", &shell_exit);
  err = errno;
  probe("wait_spawn_sleep_bg", shell_rc == 0 && shell_exit == 0, (shell_rc < 0) ? -1 : 0, (shell_rc < 0) ? err : 0);

  if(shell_rc == 0){
    memset(&si_buf, 0xff, sizeof(si_buf));
    errno = 0;
    rc = waitid(P_ALL, 0, si_buf.raw, WEXITED | WNOHANG);
    err = errno;
    probe("waitid_wnohang_noevent", rc == 0 && err == 0 && si_buf.si.si_signo == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

    memset(&si_buf, 0, sizeof(si_buf));
    errno = 0;
    rc = wait_for_exit_event(&si_buf.si, &waitid_siginfo_pid, &waitid_siginfo_status);
    err = errno;
    probe("waitid_exit_wnowait", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);
    if(rc == 0 && err == 0){
      waitid_have_siginfo = 1;

      probe("waitid_siginfo_signo",
            si_buf.si.si_signo == SIGCHLD,
            (si_buf.si.si_signo == SIGCHLD) ? 0 : -1,
            0);
      probe("waitid_siginfo_code",
            si_buf.si.si_code == CLD_EXITED,
            (si_buf.si.si_code == CLD_EXITED) ? 0 : -1,
            0);
      probe("waitid_siginfo_status_pack",
            waitid_siginfo_status == 0,
            (waitid_siginfo_status == 0) ? 0 : -1,
            0);
    } else {
      probe("waitid_siginfo_signo", 0, -1, (rc < 0) ? err : 0);
      probe("waitid_siginfo_code", 0, -1, (rc < 0) ? err : 0);
      probe("waitid_siginfo_status_pack", 0, -1, (rc < 0) ? err : 0);
    }

    errno = 0;
    pid = waitpid((pid_t)-1, &status, 0);
    err = errno;
    probe("waitpid_reap_after_wnowait",
          pid > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          (pid < 0) ? -1 : 0,
          (pid < 0) ? err : 0);
    probe("waitid_siginfo_pid_pack",
          waitid_have_siginfo && pid > 0 && waitid_siginfo_pid == (int)pid,
          (waitid_have_siginfo && pid > 0 && waitid_siginfo_pid == (int)pid) ? 0 : -1,
          0);
  } else {
    probe("waitid_wnohang_noevent", 0, -1, err);
    probe("waitid_exit_wnowait", 0, -1, err);
    probe("waitid_siginfo_signo", 0, -1, err);
    probe("waitid_siginfo_code", 0, -1, err);
    probe("waitid_siginfo_status_pack", 0, -1, err);
    probe("waitpid_reap_after_wnowait", 0, -1, err);
    probe("waitid_siginfo_pid_pack", 0, -1, err);
  }

  memset(&si_buf, 0, sizeof(si_buf));
  errno = 0;
  rc = waitid(P_ALL, 0, si_buf.raw, WEXITED | WNOHANG);
  err = errno;
  probe("waitid_nochild", rc < 0 && err == ECHILD, (rc < 0) ? -1 : 0, err);

  (void)unlink("/tmp/probe_jobs_snapshot");
  stop_pid = -1;
  stop_sig = -1;
  errno = 0;
  shell_rc = shrt_eval_line("cat /dev/tty &", &shell_exit);
  err = errno;
  if(shell_rc == 0 && shell_exit == 0 && wait_for_stop_event(&si_buf.si, &stop_pid, &stop_sig) == 0){
    probe("job_bg_tty_read_stop",
          si_buf.si.si_code == CLD_STOPPED && stop_pid > 0 && stop_sig == SIGTTIN,
          (si_buf.si.si_code == CLD_STOPPED && stop_pid > 0 && stop_sig == SIGTTIN) ? 0 : -1,
          0);

    errno = 0;
    pid = waitpid((pid_t)stop_pid, &status, WUNTRACED);
    err = errno;
    probe("waitpid_stop_sigttin",
          pid == (pid_t)stop_pid && WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTIN,
          (pid < 0) ? -1 : 0,
          (pid < 0) ? err : 0);
    (void)kill((pid_t)stop_pid, SIGKILL);
    (void)waitpid((pid_t)stop_pid, &status, 0);
  } else {
    probe("job_bg_tty_read_stop", 0, -1, (shell_rc < 0) ? err : errno);
    probe("waitpid_stop_sigttin", 0, -1, (shell_rc < 0) ? err : errno);
    if(shrt_eval_line("jobs >/tmp/probe_jobs_snapshot", &shell_exit) == 0 &&
       read_small_file("/tmp/probe_jobs_snapshot", file_buf, sizeof(file_buf)) == 0)
    {
      stop_pid = parse_first_job_id(file_buf);
      if(stop_pid > 0){
        (void)kill((pid_t)stop_pid, SIGKILL);
        (void)waitpid((pid_t)stop_pid, &status, 0);
      }
    }
  }

  errno = 0;
  fd = open("/dev/tty", O_RDWR);
  if(fd < 0 || tcgetattr(fd, &tio_saved) != 0){
    probe("job_bg_tty_write_stop", 0, -1, (fd < 0) ? errno : errno);
    probe("waitpid_stop_sigttou", 0, -1, (fd < 0) ? errno : errno);
    if(fd >= 0)
      close(fd);
  } else {
    pgid = getpgrp();
    if(pgid > 0)
      (void)tcsetpgrp(fd, pgid);
    tio_peer = tio_saved;
#ifdef TOSTOP
    tio_peer.c_lflag |= TOSTOP;
#endif
    if(tcsetattr(fd, TCSANOW, &tio_peer) != 0){
      probe("job_bg_tty_write_stop", 0, -1, errno);
      probe("waitpid_stop_sigttou", 0, -1, errno);
    } else {
      stop_pid = -1;
      stop_sig = -1;
      errno = 0;
      shell_rc = shrt_eval_line("echo bg-write >/dev/tty &", &shell_exit);
      err = errno;
      if(shell_rc == 0 && shell_exit == 0 && wait_for_stop_event(&si_buf.si, &stop_pid, &stop_sig) == 0){
        probe("job_bg_tty_write_stop",
              si_buf.si.si_code == CLD_STOPPED && stop_pid > 0 && stop_sig == SIGTTOU,
              (si_buf.si.si_code == CLD_STOPPED && stop_pid > 0 && stop_sig == SIGTTOU) ? 0 : -1,
              0);

        errno = 0;
        pid = waitpid((pid_t)stop_pid, &status, WUNTRACED);
        err = errno;
        probe("waitpid_stop_sigttou",
              pid == (pid_t)stop_pid && WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTOU,
              (pid < 0) ? -1 : 0,
              (pid < 0) ? err : 0);
        (void)kill((pid_t)stop_pid, SIGKILL);
        (void)waitpid((pid_t)stop_pid, &status, 0);
      } else {
        probe("job_bg_tty_write_stop", 0, -1, (shell_rc < 0) ? err : errno);
        probe("waitpid_stop_sigttou", 0, -1, (shell_rc < 0) ? err : errno);
        if(shrt_eval_line("jobs >/tmp/probe_jobs_snapshot", &shell_exit) == 0 &&
           read_small_file("/tmp/probe_jobs_snapshot", file_buf, sizeof(file_buf)) == 0)
        {
          stop_pid = parse_first_job_id(file_buf);
          if(stop_pid > 0){
            (void)kill((pid_t)stop_pid, SIGKILL);
            (void)waitpid((pid_t)stop_pid, &status, 0);
          }
        }
      }
    }
    (void)tcsetattr(fd, TCSANOW, &tio_saved);
    close(fd);
  }

  errno = 0;
  pgid = getpgrp();
  err = errno;
  probe("job_getpgrp_self", pgid > 0 && err == 0, (pgid < 0) ? -1 : 0, (pgid < 0) ? err : 0);

  errno = 0;
  rc = (int)getpgid((pid_t)0);
  err = errno;
  probe("job_getpgid_self", rc > 0 && err == 0 && pgid > 0 && rc == (int)pgid, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

  errno = 0;
  sid = getsid((pid_t)0);
  err = errno;
  probe("job_getsid_self", sid > 0 && err == 0, (sid < 0) ? -1 : 0, (sid < 0) ? err : 0);

  errno = 0;
  rc = setpgid((pid_t)-1, (pid_t)0);
  err = errno;
  probe("setpgid_badpid", rc < 0 && err == EINVAL, (rc < 0) ? -1 : 0, err);

  errno = 0;
  rc = setpgid((pid_t)0, (pid_t)0);
  err = errno;
  probe("setpgid_self_nop", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

  errno = 0;
  rc = (int)setsid();
  err = errno;
  probe("setsid_leader", rc < 0 && err == EPERM, (rc < 0) ? -1 : 0, err);

  errno = 0;
  fd = open("/dev/tty", O_RDWR);
  if(fd < 0){
    probe("tcgetpgrp_tty", 0, -1, errno);
    probe("tcsetpgrp_same", 0, -1, errno);
    probe("tcsetpgrp_badgrp", 0, -1, errno);
  } else {
    int fg_pgrp;
    errno = 0;
    fg_pgrp = tcgetpgrp(fd);
    err = errno;
    probe("tcgetpgrp_tty", fg_pgrp > 0 && err == 0, (fg_pgrp < 0) ? -1 : 0, (fg_pgrp < 0) ? err : 0);

    errno = 0;
    rc = tcsetpgrp(fd, (pid_t)fg_pgrp);
    err = errno;
    probe("tcsetpgrp_same", rc == 0 && err == 0, (rc < 0) ? -1 : 0, (rc < 0) ? err : 0);

    errno = 0;
    rc = tcsetpgrp(fd, (pid_t)0);
    err = errno;
    probe("tcsetpgrp_badgrp", rc < 0 && err == EINVAL, (rc < 0) ? -1 : 0, err);
    close(fd);
  }

  errno = 0;
  fd = open("/etc/rc", O_RDONLY);
  if(fd < 0){
    probe("tcgetpgrp_notty", 0, -1, errno);
    probe("tcsetpgrp_notty", 0, -1, errno);
  } else {
    errno = 0;
    rc = tcgetpgrp(fd);
    err = errno;
    probe("tcgetpgrp_notty", rc < 0 && err == ENOTTY, (rc < 0) ? -1 : 0, err);

    errno = 0;
    rc = tcsetpgrp(fd, (pgid > 0) ? pgid : (pid_t)1);
    err = errno;
    probe("tcsetpgrp_notty", rc < 0 && err == ENOTTY, (rc < 0) ? -1 : 0, err);
    close(fd);
  }

  printf("PROBE SUMMARY failures=%d\n", g_failures);
  return (g_failures == 0) ? 0 : 1;
}
