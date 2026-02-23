/**
 * @file vfs_fd.c
 * @brief File descriptor implementation
 *
 * Implements the file descriptor table for the VFS. Each FD entry
 * maps an integer file descriptor to an underlying file, device, or pipe.
 *
 * The FD table is process-local in FreeRTOS - each task has its own
 * view of file descriptors. However, the underlying file/device objects
 * (inodes, PTYs, pipes) are shared.
 *
 * FD slots 0, 1, 2 are reserved for stdin, stdout, stderr and are
 * pre-allocated during init.
 */
#include "vfs/vfs_fd.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "vfs/vfs.h"

#define XV6_MAX_FD XV6_FD_CAP

/** Global file descriptor table */
xv6_vfd_t g_fds[XV6_MAX_FD];

/** Next FD group ID for close-on-exec tracking */
static int g_next_fd_group = 1;

/** Mutex for thread-safe FD table access */
static SemaphoreHandle_t g_fd_lock;

/**
 * @brief Acquire FD table lock
 *
 * Creates the mutex if needed, then takes it.
 */
static void fd_lock(void)
{
  if(g_fd_lock == 0)
    g_fd_lock = xSemaphoreCreateMutex();
  if(g_fd_lock)
    (void)xSemaphoreTake(g_fd_lock, portMAX_DELAY);
}

/**
 * @brief Release FD table lock
 */
static void fd_unlock(void)
{
  if(g_fd_lock)
    (void)xSemaphoreGive(g_fd_lock);
}

int vfs_fd_init(void)
{
  /* Clear the entire FD table */
  memset(g_fds, 0, sizeof(g_fds));
  
  /* Reset group ID counter */
  g_next_fd_group = 1;
  
  return 0;
}

int vfs_fd_alloc(void)
{
  int i;
  
  fd_lock();
  
  /* Start from 3 (0,1,2 are stdin/stdout/stderr) */
  for(i = 3; i < XV6_MAX_FD; i++){
    if(!g_fds[i].used){
      /* Found a free slot - mark it as used */
      g_fds[i].used = 1;
      g_fds[i].off = 0;  /* Initialize file offset */
      fd_unlock();
      return i;
    }
  }
  
  fd_unlock();
  return -1;  /* No free slots */
}

xv6_vfd_t *vfs_fd_get(int fd)
{
  /* Validate FD range */
  if(fd < 0 || fd >= XV6_MAX_FD)
    return NULL;
    
  /* Check if slot is in use */
  if(!g_fds[fd].used)
    return NULL;
    
  return &g_fds[fd];
}

void vfs_fd_free(int fd)
{
  /* Validate FD range */
  if(fd < 0 || fd >= XV6_MAX_FD)
    return;
    
  fd_lock();
  
  /* Clear the FD entry */
  memset(&g_fds[fd], 0, sizeof(g_fds[fd]));
  
  fd_unlock();
}

int vfs_fd_dup(int oldfd)
{
  int i;
  
  fd_lock();
  
  /* Validate old FD */
  if(oldfd < 0 || oldfd >= XV6_MAX_FD || !g_fds[oldfd].used){
    fd_unlock();
    return -1;
  }
  
  /* Find first free slot without re-entering the same mutex */
  for(i = 3; i < XV6_MAX_FD; i++){
    if(!g_fds[i].used){
      g_fds[i] = g_fds[oldfd];
      g_fds[i].used = 1;
      fd_unlock();
      return i;
    }
  }

  fd_unlock();
  return -1;
}
