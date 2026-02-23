/**
 * @file vfs_fd.c
 * @brief File descriptor implementation
 */
#include "vfs/vfs_fd.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "vfs/xv6fs_ro.h"

#define XV6_MAX_FD XV6_FD_CAP

static xv6_vfd_t g_fds[XV6_MAX_FD];
static int g_next_fd_group = 1;
static SemaphoreHandle_t g_fd_lock;

static void fd_lock(void)
{
  if(g_fd_lock == 0)
    g_fd_lock = xSemaphoreCreateMutex();
  if(g_fd_lock)
    (void)xSemaphoreTake(g_fd_lock, portMAX_DELAY);
}

static void fd_unlock(void)
{
  if(g_fd_lock)
    (void)xSemaphoreGive(g_fd_lock);
}

int vfs_fd_init(void)
{
  memset(g_fds, 0, sizeof(g_fds));
  g_next_fd_group = 1;
  return 0;
}

int vfs_fd_alloc(void)
{
  int i;
  fd_lock();
  for(i = 3; i < XV6_MAX_FD; i++){
    if(!g_fds[i].used){
      g_fds[i].used = 1;
      g_fds[i].off = 0;
      fd_unlock();
      return i;
    }
  }
  fd_unlock();
  return -1;
}

xv6_vfd_t *vfs_fd_get(int fd)
{
  if(fd < 0 || fd >= XV6_MAX_FD)
    return NULL;
  if(!g_fds[fd].used)
    return NULL;
  return &g_fds[fd];
}

void vfs_fd_free(int fd)
{
  if(fd < 0 || fd >= XV6_MAX_FD)
    return;
  fd_lock();
  memset(&g_fds[fd], 0, sizeof(g_fds[fd]));
  fd_unlock();
}

int vfs_fd_dup(int oldfd)
{
  int newfd;
  fd_lock();
  if(oldfd < 0 || oldfd >= XV6_MAX_FD || !g_fds[oldfd].used){
    fd_unlock();
    return -1;
  }
  newfd = vfs_fd_alloc();
  if(newfd < 0){
    fd_unlock();
    return -1;
  }
  g_fds[newfd] = g_fds[oldfd];
  fd_unlock();
  return newfd;
}
