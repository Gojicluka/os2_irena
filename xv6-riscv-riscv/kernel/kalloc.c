#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "slab.h"

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

void
kinit()
{
  char *start = (char *)PGROUNDUP((uint64)end);
  kmem_init(start, ((char *)PHYSTOP - start) / PGSIZE);
}
