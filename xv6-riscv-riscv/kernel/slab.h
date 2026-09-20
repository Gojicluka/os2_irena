#ifndef XV6_SLAB_H
#define XV6_SLAB_H

typedef struct kmem_cache_s kmem_cache_t;

#define BLOCK_SIZE 4096

typedef unsigned long size_t;

void kmem_init(void *space, int block_num);
kmem_cache_t *kmem_cache_create(const char *name, size_t size,
                                void (*ctor)(void *),
                                void (*dtor)(void *));
int kmem_cache_shrink(kmem_cache_t *cachep);
void *kmem_cache_alloc(kmem_cache_t *cachep);
void kmem_cache_free(kmem_cache_t *cachep, void *objp);
void *kmalloc(size_t size);
void kfree(const void *objp);
void kmem_cache_destroy(kmem_cache_t *cachep);
void kmem_cache_info(kmem_cache_t *cachep);
int kmem_cache_error(kmem_cache_t *cachep);

#endif
