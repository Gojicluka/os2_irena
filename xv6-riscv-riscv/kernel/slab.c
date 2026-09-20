#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "slab.h"

#define MAX_BUDDY_ORDER 20
#define KMALLOC_MIN_ORDER 5
#define KMALLOC_MAX_ORDER 17
#define KMALLOC_CACHE_COUNT (KMALLOC_MAX_ORDER - KMALLOC_MIN_ORDER + 1)
#define CACHE_NAME_LEN 32
#define MAP_INTERIOR (-128)

enum cache_error {
  CACHE_OK,
  CACHE_BAD_ARGUMENT,
  CACHE_NO_MEMORY,
  CACHE_BAD_OBJECT,
  CACHE_BUSY
};

struct buddy_node {
  struct buddy_node *next;
};

struct slab_s {
  struct slab_s *next;
  uint used;
  uint capacity;
  uint object_offset;
};

struct kmem_cache_s {
  struct spinlock lock;
  struct kmem_cache_s *next;
  struct slab_s *empty;
  struct slab_s *partial;
  struct slab_s *full;
  size_t requested_size;
  size_t object_size;
  uint slab_order;
  uint slab_count;
  uint object_count;
  int grew;
  int error;
  void (*ctor)(void *);
  void (*dtor)(void *);
  char name[CACHE_NAME_LEN];
};

struct kmem_manager {
  struct spinlock buddy_lock;
  struct spinlock cache_lock;
  struct buddy_node *free[MAX_BUDDY_ORDER + 1];
  kmem_cache_t *caches;
  kmem_cache_t *small[KMALLOC_CACHE_COUNT];
  char *base;
  signed char *map;
  uint blocks;
  uint max_order;
};

static struct kmem_manager *manager;

static uint
ceil_log2(uint n)
{
  uint order = 0;
  uint value = 1;

  while(value < n){
    value <<= 1;
    order++;
  }
  return order;
}

static uint64
align_up(uint64 value, uint64 alignment)
{
  return (value + alignment - 1) & ~(alignment - 1);
}

static void
buddy_add(uint index, uint order)
{
  struct buddy_node *node = (struct buddy_node *)(manager->base +
                                                  index * BLOCK_SIZE);
  node->next = manager->free[order];
  manager->free[order] = node;
  manager->map[index] = order;
}

static int
buddy_remove(uint index, uint order)
{
  struct buddy_node **link = &manager->free[order];
  struct buddy_node *target = (struct buddy_node *)(manager->base +
                                                    index * BLOCK_SIZE);

  while(*link){
    if(*link == target){
      *link = target->next;
      manager->map[index] = MAP_INTERIOR;
      return 0;
    }
    link = &(*link)->next;
  }
  return -1;
}

static void *
buddy_alloc(uint order)
{
  struct buddy_node *node;
  uint found;
  uint index;

  if(manager == 0 || order > manager->max_order)
    return 0;

  acquire(&manager->buddy_lock);
  for(found = order; found <= manager->max_order; found++)
    if(manager->free[found])
      break;
  if(found > manager->max_order){
    release(&manager->buddy_lock);
    return 0;
  }

  node = manager->free[found];
  manager->free[found] = node->next;
  index = ((char *)node - manager->base) / BLOCK_SIZE;
  manager->map[index] = MAP_INTERIOR;

  while(found > order){
    uint right;
    found--;
    right = index + (1U << found);
    buddy_add(right, found);
  }
  manager->map[index] = -(signed char)(order + 2);
  release(&manager->buddy_lock);

  return manager->base + index * BLOCK_SIZE;
}

static int
buddy_free(void *address, uint order)
{
  uint index;

  if(manager == 0 || address == 0 ||
     (char *)address < manager->base ||
     (char *)address >= manager->base + manager->blocks * BLOCK_SIZE ||
     ((uint64)address % BLOCK_SIZE) != 0 || order > manager->max_order)
    return -1;

  index = ((char *)address - manager->base) / BLOCK_SIZE;
  acquire(&manager->buddy_lock);
  if(manager->map[index] != -(signed char)(order + 2)){
    release(&manager->buddy_lock);
    return -1;
  }

  manager->map[index] = MAP_INTERIOR;
  while(order < manager->max_order){
    uint buddy = index ^ (1U << order);
    if(buddy >= manager->blocks || manager->map[buddy] != order)
      break;
    if(buddy_remove(buddy, order) < 0)
      break;
    if(buddy < index)
      index = buddy;
    order++;
  }
  buddy_add(index, order);
  release(&manager->buddy_lock);
  return 0;
}

void
kmem_init(void *space, int block_num)
{
  uint64 bytes;
  uint metadata_blocks;
  uint usable;
  uint index;
  uint remaining;

  if(manager || space == 0 || block_num <= 0)
    return;

  bytes = sizeof(struct kmem_manager) + (uint64)block_num;
  metadata_blocks = (bytes + BLOCK_SIZE - 1) / BLOCK_SIZE;
  if(metadata_blocks >= (uint)block_num)
    return;

  manager = (struct kmem_manager *)space;
  memset(manager, 0, metadata_blocks * BLOCK_SIZE);
  manager->map = (signed char *)manager + sizeof(*manager);
  manager->base = (char *)space + metadata_blocks * BLOCK_SIZE;
  usable = block_num - metadata_blocks;
  manager->blocks = usable;
  manager->max_order = ceil_log2(usable);
  if((1U << manager->max_order) > usable)
    manager->max_order--;
  if(manager->max_order > MAX_BUDDY_ORDER)
    manager->max_order = MAX_BUDDY_ORDER;

  initlock(&manager->buddy_lock, "buddy");
  initlock(&manager->cache_lock, "slab-list");
  memset(manager->map, MAP_INTERIOR, usable);

  index = 0;
  remaining = usable;
  while(remaining){
    uint order = manager->max_order;
    while((1U << order) > remaining || (index & ((1U << order) - 1)) != 0)
      order--;
    buddy_add(index, order);
    index += 1U << order;
    remaining -= 1U << order;
  }
}

void *
kalloc(void)
{
  void *page = buddy_alloc(0);
  if(page)
    memset(page, 5, BLOCK_SIZE);
  return page;
}

void
kpage_free(void *page)
{
  if(page == 0 || ((uint64)page % BLOCK_SIZE) != 0)
    panic("kpage_free");
  memset(page, 1, BLOCK_SIZE);
  if(buddy_free(page, 0) < 0)
    panic("kpage_free");
}

static uint
slab_capacity(size_t object_size, uint order)
{
  uint64 bytes = (uint64)BLOCK_SIZE << order;
  uint capacity;

  if(bytes <= sizeof(struct slab_s))
    return 0;
  capacity = (bytes - sizeof(struct slab_s)) / object_size;
  while(capacity &&
        align_up(sizeof(struct slab_s) + (capacity + 7) / 8, 16) +
          (uint64)capacity * object_size > bytes)
    capacity--;
  return capacity;
}

static void
list_push(struct slab_s **list, struct slab_s *slab)
{
  slab->next = *list;
  *list = slab;
}

static void
list_remove(struct slab_s **list, struct slab_s *slab)
{
  while(*list){
    if(*list == slab){
      *list = slab->next;
      slab->next = 0;
      return;
    }
    list = &(*list)->next;
  }
}

static struct slab_s *
slab_create(kmem_cache_t *cache)
{
  struct slab_s *slab;
  char *object;
  uchar *bitmap;
  uint i;

  slab = buddy_alloc(cache->slab_order);
  if(slab == 0){
    cache->error = CACHE_NO_MEMORY;
    return 0;
  }

  memset(slab, 0, (uint64)BLOCK_SIZE << cache->slab_order);
  slab->capacity = slab_capacity(cache->object_size, cache->slab_order);
  slab->object_offset = align_up(sizeof(*slab) + (slab->capacity + 7) / 8,
                                 16);
  bitmap = (uchar *)slab + sizeof(*slab);
  memset(bitmap, 0, (slab->capacity + 7) / 8);
  object = (char *)slab + slab->object_offset;
  for(i = 0; i < slab->capacity; i++, object += cache->object_size){
    if(cache->ctor)
      cache->ctor(object);
  }

  cache->slab_count++;
  cache->object_count += slab->capacity;
  cache->grew = 1;
  list_push(&cache->empty, slab);
  return slab;
}

static void
slab_release(kmem_cache_t *cache, struct slab_s *slab)
{
  char *object = (char *)slab + slab->object_offset;
  uint i;

  if(cache->dtor)
    for(i = 0; i < slab->capacity; i++, object += cache->object_size)
      cache->dtor(object);
  cache->slab_count--;
  cache->object_count -= slab->capacity;
  if(buddy_free(slab, cache->slab_order) < 0)
    panic("slab_release");
}

static int
cache_is_registered(kmem_cache_t *cache)
{
  kmem_cache_t *current;

  if(manager == 0 || cache == 0)
    return 0;
  for(current = manager->caches; current; current = current->next)
    if(current == cache)
      return 1;
  return 0;
}

static int
cache_lock(kmem_cache_t *cache)
{
  if(manager == 0 || cache == 0)
    return -1;
  acquire(&manager->cache_lock);
  if(!cache_is_registered(cache)){
    release(&manager->cache_lock);
    return -1;
  }
  acquire(&cache->lock);
  release(&manager->cache_lock);
  return 0;
}

kmem_cache_t *
kmem_cache_create(const char *name, size_t size, void (*ctor)(void *),
                  void (*dtor)(void *))
{
  kmem_cache_t *cache;
  uint blocks;
  uint name_length;

  if(manager == 0 || name == 0 || size == 0)
    return 0;

  cache = buddy_alloc(0);
  if(cache == 0)
    return 0;
  memset(cache, 0, BLOCK_SIZE);
  initlock(&cache->lock, "slab-cache");
  cache->requested_size = size;
  cache->object_size = align_up(size < sizeof(void *) ? sizeof(void *) : size,
                                16);
  cache->ctor = ctor;
  cache->dtor = dtor;

  blocks = (align_up(sizeof(struct slab_s), 16) + cache->object_size +
            BLOCK_SIZE - 1) / BLOCK_SIZE;
  cache->slab_order = ceil_log2(blocks);
  if(cache->slab_order > manager->max_order ||
     slab_capacity(cache->object_size, cache->slab_order) == 0){
    buddy_free(cache, 0);
    return 0;
  }

  name_length = strlen(name);
  if(name_length >= CACHE_NAME_LEN)
    name_length = CACHE_NAME_LEN - 1;
  memmove(cache->name, name, name_length);
  cache->name[name_length] = 0;

  acquire(&manager->cache_lock);
  cache->next = manager->caches;
  manager->caches = cache;
  release(&manager->cache_lock);
  return cache;
}

void *
kmem_cache_alloc(kmem_cache_t *cache)
{
  struct slab_s *slab;
  void *object;
  uchar *bitmap;
  uint index;

  if(cache_lock(cache) < 0)
    return 0;

  slab = cache->partial;
  if(slab == 0)
    slab = cache->empty;
  if(slab == 0)
    slab = slab_create(cache);
  if(slab == 0){
    release(&cache->lock);
    return 0;
  }

  if(slab->used == 0){
    list_remove(&cache->empty, slab);
    list_push(&cache->partial, slab);
  }
  bitmap = (uchar *)slab + sizeof(*slab);
  for(index = 0; index < slab->capacity; index++)
    if((bitmap[index / 8] & (1U << (index % 8))) == 0)
      break;
  if(index == slab->capacity)
    panic("slab bitmap");
  bitmap[index / 8] |= 1U << (index % 8);
  object = (char *)slab + slab->object_offset +
           index * cache->object_size;
  slab->used++;
  if(slab->used == slab->capacity){
    list_remove(&cache->partial, slab);
    list_push(&cache->full, slab);
  }
  cache->error = CACHE_OK;
  release(&cache->lock);
  return object;
}

static struct slab_s *
find_slab(kmem_cache_t *cache, const void *object)
{
  struct slab_s *lists[3] = {cache->empty, cache->partial, cache->full};
  uint i;

  for(i = 0; i < NELEM(lists); i++){
    struct slab_s *slab;
    for(slab = lists[i]; slab; slab = slab->next){
      char *first = (char *)slab + slab->object_offset;
      char *limit = first + slab->capacity * cache->object_size;
      if((char *)object >= first && (char *)object < limit &&
         ((char *)object - first) % cache->object_size == 0)
        return slab;
    }
  }
  return 0;
}

static void
cache_free_locked(kmem_cache_t *cache, struct slab_s *slab, void *object)
{
  uchar *bitmap;
  char *first;
  uint index;

  first = (char *)slab + slab->object_offset;
  index = ((char *)object - first) / cache->object_size;
  bitmap = (uchar *)slab + sizeof(*slab);
  if((bitmap[index / 8] & (1U << (index % 8))) == 0){
    cache->error = CACHE_BAD_OBJECT;
    return;
  }

  if(slab->used == slab->capacity){
    list_remove(&cache->full, slab);
    list_push(&cache->partial, slab);
  }
  bitmap[index / 8] &= ~(1U << (index % 8));
  slab->used--;
  if(slab->used == 0){
    list_remove(&cache->partial, slab);
    list_push(&cache->empty, slab);
  }
  cache->error = CACHE_OK;
}

void
kmem_cache_free(kmem_cache_t *cache, void *object)
{
  struct slab_s *slab;

  if(object == 0 || cache_lock(cache) < 0)
    return;
  slab = find_slab(cache, object);
  if(slab == 0)
    cache->error = CACHE_BAD_OBJECT;
  else
    cache_free_locked(cache, slab, object);
  release(&cache->lock);
}

int
kmem_cache_shrink(kmem_cache_t *cache)
{
  struct slab_s *slab;
  int released = 0;

  if(cache_lock(cache) < 0)
    return 0;
  if(cache->grew){
    cache->grew = 0;
    release(&cache->lock);
    return 0;
  }

  while((slab = cache->empty) != 0){
    cache->empty = slab->next;
    released += 1U << cache->slab_order;
    slab_release(cache, slab);
  }
  cache->error = CACHE_OK;
  release(&cache->lock);
  return released;
}

void
kmem_cache_destroy(kmem_cache_t *cache)
{
  kmem_cache_t **link;
  struct slab_s *slab;
  uint i;

  if(manager == 0 || cache == 0)
    return;
  acquire(&manager->cache_lock);
  for(link = &manager->caches; *link && *link != cache; link = &(*link)->next)
    ;
  if(*link == 0){
    release(&manager->cache_lock);
    return;
  }
  acquire(&cache->lock);
  if(cache->partial || cache->full){
    cache->error = CACHE_BUSY;
    release(&cache->lock);
    release(&manager->cache_lock);
    return;
  }
  *link = cache->next;
  for(i = 0; i < KMALLOC_CACHE_COUNT; i++)
    if(manager->small[i] == cache)
      manager->small[i] = 0;
  release(&manager->cache_lock);

  while((slab = cache->empty) != 0){
    cache->empty = slab->next;
    slab_release(cache, slab);
  }
  release(&cache->lock);
  if(buddy_free(cache, 0) < 0)
    panic("cache_destroy");
}

void *
kmalloc(size_t size)
{
  uint order;
  uint index;
  kmem_cache_t *cache;
  char name[CACHE_NAME_LEN];

  if(manager == 0 || size == 0 || size > (1UL << KMALLOC_MAX_ORDER))
    return 0;
  order = KMALLOC_MIN_ORDER;
  while((1UL << order) < size)
    order++;
  index = order - KMALLOC_MIN_ORDER;

  acquire(&manager->cache_lock);
  cache = manager->small[index];
  release(&manager->cache_lock);
  if(cache == 0){
    name[0] = 's';
    name[1] = 'i';
    name[2] = 'z';
    name[3] = 'e';
    name[4] = '-';
    {
      uint value = 1U << order;
      char digits[12];
      int count = 0;
      int pos = 5;
      do {
        digits[count++] = '0' + value % 10;
        value /= 10;
      } while(value);
      while(count)
        name[pos++] = digits[--count];
      name[pos] = 0;
    }
    cache = kmem_cache_create(name, 1UL << order, 0, 0);
    if(cache == 0)
      return 0;
    acquire(&manager->cache_lock);
    if(manager->small[index] == 0)
      manager->small[index] = cache;
    else {
      kmem_cache_t *existing = manager->small[index];
      release(&manager->cache_lock);
      kmem_cache_destroy(cache);
      cache = existing;
      return kmem_cache_alloc(cache);
    }
    release(&manager->cache_lock);
  }
  return kmem_cache_alloc(cache);
}

void
kfree(const void *object)
{
  uint i;

  if(manager == 0 || object == 0)
    return;
  acquire(&manager->cache_lock);
  for(i = 0; i < KMALLOC_CACHE_COUNT; i++){
    kmem_cache_t *cache = manager->small[i];
    struct slab_s *slab;
    if(cache == 0)
      continue;
    acquire(&cache->lock);
    slab = find_slab(cache, object);
    if(slab){
      cache_free_locked(cache, slab, (void *)object);
      if(cache->empty && cache->empty->next){
        slab = cache->empty;
        cache->empty = slab->next;
        slab_release(cache, slab);
      }
      release(&cache->lock);
      release(&manager->cache_lock);
      return;
    }
    release(&cache->lock);
  }
  release(&manager->cache_lock);
}

void
kmem_cache_info(kmem_cache_t *cache)
{
  struct slab_s *slab;
  uint used = 0;
  uint percent;

  if(cache_lock(cache) < 0)
    return;
  for(slab = cache->partial; slab; slab = slab->next)
    used += slab->used;
  for(slab = cache->full; slab; slab = slab->next)
    used += slab->used;
  percent = cache->object_count ? (used * 100) / cache->object_count : 0;
  printf("cache %s: object=%lu bytes, blocks=%u, slabs=%u, "
         "objects/slab=%u, used=%u%%\n",
         cache->name, cache->requested_size,
         cache->slab_count * (1U << cache->slab_order),
         cache->slab_count,
         slab_capacity(cache->object_size, cache->slab_order), percent);
  release(&cache->lock);
}

int
kmem_cache_error(kmem_cache_t *cache)
{
  int error;
  char *message;

  if(cache_lock(cache) < 0){
    printf("cache error: invalid cache\n");
    return CACHE_BAD_ARGUMENT;
  }
  error = cache->error;
  switch(error){
  case CACHE_OK:
    message = "no error";
    break;
  case CACHE_BAD_ARGUMENT:
    message = "bad argument";
    break;
  case CACHE_NO_MEMORY:
    message = "out of memory";
    break;
  case CACHE_BAD_OBJECT:
    message = "invalid object";
    break;
  case CACHE_BUSY:
    message = "cache busy";
    break;
  default:
    message = "unknown error";
    break;
  }
  printf("cache %s: %s\n", cache->name, message);
  release(&cache->lock);
  return error;
}
