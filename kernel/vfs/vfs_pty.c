/**
 * @file vfs_pty.c
 * @brief PTY (pseudo-terminal) implementation
 *
 * Implements PTY (pseudo-terminal) support for terminal emulation.
 * A PTY consists of a master and slave pair - the master is used by
 * the controlling process (e.g., a shell), and the slave appears as
 * a regular terminal to the subordinate process.
 *
 * Data flow:
 * - Data written to master appears as input to slave (m2s queue)
 * - Data written to slave appears as input to master (s2m queue)
 *
 * The implementation uses circular buffers for each direction.
 */
#include "vfs/vfs_pty.h"

#include <stdio.h>
#include <string.h>

#include "vfs/xv6fs_ro.h"

#define XV6_MAX_PTY XV6_PTY_CAP

/**
 * @brief PTY slot structure
 *
 * Each PTY has two unidirectional queues for data transfer:
 * - m2s: master -> slave (data from shell to terminal)
 * - s2m: slave -> master (data from terminal to shell)
 */
typedef struct {
  int alloc;          /**< Allocation flag */
  int master_open;    /**< Master reference count */
  int slave_open;     /**< Slave reference count */
  
  /* Master-to-slave queue: data written to master appears on slave */
  uint8 m2s[256];
  uint16 m2s_r;       /**< Read position */
  uint16 m2s_w;       /**< Write position */
  uint16 m2s_n;       /**< Bytes in queue */
  
  /* Slave-to-master queue: data written to slave appears on master */
  uint8 s2m[256];
  uint16 s2m_r;       /**< Read position */
  uint16 s2m_w;       /**< Write position */
  uint16 s2m_n;       /**< Bytes in queue */
} xv6_pty_t;

/** Global PTY table */
static xv6_pty_t g_ptys[XV6_MAX_PTY];

/**
 * @brief Push data onto a circular queue
 *
 * Copies data from source to a circular buffer, advancing the write
 * pointer and count for each byte written. Stops when buffer is full.
 *
 * @param buf       Circular buffer
 * @param w         Write position pointer
 * @param n         Count pointer
 * @param cap       Buffer capacity
 * @param src       Source data
 * @param size      Bytes to copy
 * @return Bytes actually written
 */
static int pty_q_push(uint8 *buf, uint16 *w, uint16 *n, uint16 cap, const uint8 *src, uint32 size)
{
  uint32 i;
  uint32 wrote = 0;
  
  for(i = 0; i < size; i++){
    /* Stop if queue is full */
    if(*n >= cap)
      break;
    
    /* Write byte and advance write position (wrap with modulo) */
    buf[*w] = src[i];
    *w = (uint16)((*w + 1) % cap);
    (*n)++;
    wrote++;
  }
  
  return (int)wrote;
}

/**
 * @brief Pop data from a circular queue
 *
 * Copies data from a circular buffer to destination, advancing the read
 * pointer and decrementing count for each byte read. Stops when queue empty.
 *
 * @param buf       Circular buffer
 * @param r         Read position pointer
 * @param n         Count pointer
 * @param cap       Buffer capacity
 * @param dst       Destination buffer
 * @param size      Max bytes to read
 * @return Bytes actually read
 */
static int pty_q_pop(uint8 *buf, uint16 *r, uint16 *n, uint16 cap, uint8 *dst, uint32 size)
{
  uint32 i;
  uint32 out = 0;
  
  for(i = 0; i < size; i++){
    /* Stop if queue is empty */
    if(*n == 0)
      break;
    
    /* Read byte and advance read position (wrap with modulo) */
    dst[i] = buf[*r];
    *r = (uint16)((*r + 1) % cap);
    (*n)--;
    out++;
  }
  
  return (int)out;
}

int vfs_pty_init(void)
{
  /* Initialize all PTY slots to zero state */
  memset(g_ptys, 0, sizeof(g_ptys));
  return 0;
}

int vfs_pty_alloc(int *master_out)
{
  int i;
  
  if(master_out == 0)
    return -1;
  
  /* Find first unallocated PTY slot */
  for(i = 0; i < XV6_MAX_PTY; i++){
    if(!g_ptys[i].alloc){
      /* Initialize the PTY slot */
      memset(&g_ptys[i], 0, sizeof(g_ptys[i]));
      g_ptys[i].alloc = 1;
      g_ptys[i].master_open = 1;  /* Master is open when allocated */
      *master_out = i;
      return 0;
    }
  }
  
  /* No free slots available */
  return -1;
}

int vfs_pty_slave_name(int master_fd, char *out, int len)
{
  /* Validate parameters */
  if(out == 0 || len <= 0)
    return -1;
  
  /* Check if master FD is valid */
  if(master_fd < 0 || master_fd >= XV6_MAX_PTY || !g_ptys[master_fd].alloc)
    return -1;
  
  /* Generate slave path: /dev/pts/N */
  snprintf(out, len, "/dev/pts/%d", master_fd);
  return 0;
}

void vfs_pty_close(int id)
{
  /* Validate PTY ID */
  if(id < 0 || id >= XV6_MAX_PTY || !g_ptys[id].alloc)
    return;
  
  /* Decrement reference counts for both ends */
  if(g_ptys[id].master_open > 0)
    g_ptys[id].master_open--;
    
  if(g_ptys[id].slave_open > 0)
    g_ptys[id].slave_open--;
  
  /* If both ends are closed, clear queued data to prevent stale reads */
  if(g_ptys[id].master_open == 0 && g_ptys[id].slave_open == 0){
    g_ptys[id].m2s_r = g_ptys[id].m2s_w = g_ptys[id].m2s_n = 0;
    g_ptys[id].s2m_r = g_ptys[id].s2m_w = g_ptys[id].s2m_n = 0;
  }
  
  /* Free the PTY if no references and no queued data */
  if(!g_ptys[id].master_open && !g_ptys[id].slave_open && 
     g_ptys[id].m2s_n == 0 && g_ptys[id].s2m_n == 0)
    memset(&g_ptys[id], 0, sizeof(g_ptys[id]));
}

int vfs_pty_read(int id, int is_master, void *buf, uint32 size)
{
  xv6_pty_t *p;
  
  /* Validate PTY ID */
  if(id < 0 || id >= XV6_MAX_PTY || !g_ptys[id].alloc)
    return -1;
  
  p = &g_ptys[id];
  
  /* Read from appropriate queue based on direction:
   * - Master reads from s2m (slave wrote to slave, master reads)
   * - Slave reads from m2s (master wrote to master, slave reads) */
  if(is_master)
    return pty_q_pop(p->s2m, &p->s2m_r, &p->s2m_n, sizeof(p->s2m), (uint8 *)buf, size);
  return pty_q_pop(p->m2s, &p->m2s_r, &p->m2s_n, sizeof(p->m2s), (uint8 *)buf, size);
}

int vfs_pty_write(int id, int is_master, const void *buf, uint32 size)
{
  xv6_pty_t *p;
  
  /* Validate PTY ID */
  if(id < 0 || id >= XV6_MAX_PTY || !g_ptys[id].alloc)
    return -1;
  
  p = &g_ptys[id];
  
  /* Write to appropriate queue based on direction:
   * - Master writes to m2s (master writes, slave reads)
   * - Slave writes to s2m (slave writes, master reads) */
  if(is_master)
    return pty_q_push(p->m2s, &p->m2s_w, &p->m2s_n, sizeof(p->m2s), (const uint8 *)buf, size);
  return pty_q_push(p->s2m, &p->s2m_w, &p->s2m_n, sizeof(p->s2m), (const uint8 *)buf, size);
}
