// Create a zombie process that
// must be reparented at exit.

#include "kernel/core/types.h"
#include "kernel/fs/stat.h"
#include "user/user.h"

int
main(void)
{
  if(fork() > 0)
    pause(5);  // Let child exit before parent.
  exit(0);
}
