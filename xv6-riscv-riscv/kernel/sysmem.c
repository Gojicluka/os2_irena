#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "slab.h"

#define USER_CACHE_NAME_LEN 32

uint64
sys_kmem_init(void)
{
  // The kernel arena is initialized during boot and cannot be replaced safely.
  return 0;
}

uint64
sys_kmem_cache_create(void)
{
  char name[USER_CACHE_NAME_LEN];
  uint64 size;
  uint64 ctor;
  uint64 dtor;

  if(argstr(0, name, sizeof(name)) < 0)
    return 0;
  argaddr(1, &size);
  argaddr(2, &ctor);
  argaddr(3, &dtor);
  if(ctor || dtor)
    return 0;
  return (uint64)kmem_cache_create(name, size, 0, 0);
}

uint64
sys_kmem_cache_shrink(void)
{
  uint64 cache;
  argaddr(0, &cache);
  return kmem_cache_shrink((kmem_cache_t *)cache);
}

uint64
sys_kmem_cache_alloc(void)
{
  uint64 cache;
  argaddr(0, &cache);
  return (uint64)kmem_cache_alloc((kmem_cache_t *)cache);
}

uint64
sys_kmem_cache_free(void)
{
  uint64 cache;
  uint64 object;
  argaddr(0, &cache);
  argaddr(1, &object);
  kmem_cache_free((kmem_cache_t *)cache, (void *)object);
  return 0;
}

uint64
sys_kmalloc(void)
{
  uint64 size;
  argaddr(0, &size);
  return (uint64)kmalloc(size);
}

uint64
sys_kfree(void)
{
  uint64 object;
  argaddr(0, &object);
  kfree((void *)object);
  return 0;
}

uint64
sys_kmem_cache_destroy(void)
{
  uint64 cache;
  argaddr(0, &cache);
  kmem_cache_destroy((kmem_cache_t *)cache);
  return 0;
}

uint64
sys_kmem_cache_info(void)
{
  uint64 cache;
  argaddr(0, &cache);
  kmem_cache_info((kmem_cache_t *)cache);
  return 0;
}

uint64
sys_kmem_cache_error(void)
{
  uint64 cache;
  argaddr(0, &cache);
  return kmem_cache_error((kmem_cache_t *)cache);
}
