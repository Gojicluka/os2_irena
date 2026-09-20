# xv6 Buddy and Slab Allocator Project

This document describes the implementation made for the OS2 2026 project
assignment, the xv6 subsystems that were migrated to the new allocator, the
public kernel/user interface, the design assumptions, and the validation that
was performed.

## 1. Overview

The original xv6 physical page free list was replaced with a unified memory
allocation subsystem consisting of:

- a buddy allocator for contiguous groups of 4096-byte blocks;
- a slab allocator for typed kernel objects;
- automatically created size caches for small buffers;
- a one-page compatibility API used by xv6 virtual-memory code;
- system calls that expose the requested allocator operations to user
  programs;
- object caches and small-buffer allocations used by existing xv6
  subsystems.

All allocator metadata is stored inside the physical-memory region assigned to
the allocator. The implementation does not call a host `malloc`, `free`, or
another external allocator.

The default xv6 configuration remains single-core (`CPUS=1`), but the
allocator protects shared state with spinlocks.

## 2. New Files

### `kernel/slab.h`

Defines the requested allocator API:

- `kmem_init`
- `kmem_cache_create`
- `kmem_cache_shrink`
- `kmem_cache_alloc`
- `kmem_cache_free`
- `kmalloc`
- `kfree`
- `kmem_cache_destroy`
- `kmem_cache_info`
- `kmem_cache_error`

It also defines the opaque `kmem_cache_t` type and the 4096-byte block size.

### `kernel/slab.c`

Contains the buddy allocator, slab allocator, size caches, page-allocation
compatibility functions, synchronization, diagnostics, and error tracking.

### `kernel/sysmem.c`

Contains the system-call wrappers for the allocator API.

### `slab.h`

Provides the user-visible allocator header by including the canonical
`kernel/slab.h` interface.

## 3. Buddy Allocator

### 3.1 Managed memory region

During boot, `kinit()` rounds the linker symbol `end` to the next page and
passes all remaining memory up to `PHYSTOP` to `kmem_init()`.

The start of this region stores:

- the global allocator manager;
- buddy free-list heads;
- the list of registered caches;
- pointers to the size caches;
- one signed-byte map entry for every managed block.

The remaining aligned blocks form the buddy allocation arena.

### 3.2 Orders and blocks

Order `n` represents `2^n` contiguous 4096-byte blocks. Allocation searches
for the smallest available order that satisfies the request. A larger block is
repeatedly split, with the unused right half inserted into the appropriate
free list.

When memory is released, the allocator calculates the buddy index with XOR.
Free buddies of the same order are removed from their free lists and merged
until no further merge is possible.

### 3.3 Allocation map

The per-block map distinguishes:

- the first block of a free buddy region and its order;
- the first block of an allocated buddy region and its order;
- interior blocks that are not valid allocation heads.

This allows the allocator to reject invalid page frees and prevents merging
with a region of the wrong size.

### 3.4 xv6 page compatibility

Existing page-oriented xv6 code still calls `kalloc()`. It now obtains one
order-zero block from the buddy allocator.

The old page version of `kfree()` was renamed to `kpage_free()`. This avoids a
name collision with the project API, where `kfree()` must free buffers created
by `kmalloc()`.

Page releases in `vm.c`, `proc.c`, and `sysfile.c` were changed to use
`kpage_free()`.

## 4. Slab Allocator

### 4.1 Cache metadata

Each cache stores:

- its name;
- requested and internally aligned object sizes;
- optional constructor and destructor functions;
- slab order;
- number of slabs and objects;
- the last cache error;
- a flag recording whether the cache grew since its last shrink request;
- separate empty, partial, and full slab lists;
- a per-cache spinlock.

Cache descriptors themselves occupy one buddy-allocated page.

### 4.2 Slab layout

Each slab contains:

1. a slab header;
2. an allocation bitmap;
3. padding for 16-byte alignment;
4. the object storage area.

The bitmap records which object slots are allocated. This avoids storing a
free-list pointer inside a released object and therefore preserves the
constructor-initialized object contents while the object is cached.

### 4.3 Slab lists

Every cache maintains the three lists required by the assignment:

- **empty**: no allocated objects;
- **partial**: at least one free and one allocated object;
- **full**: no free objects.

Allocation prefers a partial slab, then an empty slab. If neither exists, a
new slab is requested from the buddy allocator.

Freeing an object validates that:

- the object belongs to one of the cache's slabs;
- the address is aligned to an object boundary;
- the corresponding bitmap bit is currently allocated.

The slab is then moved between full, partial, and empty lists as necessary.

### 4.4 Constructors and destructors

The constructor is called for every object when a slab is created.

Freed objects remain initialized inside the slab and are reused without
calling the constructor again.

The destructor is called for every object when the slab itself is returned to
the buddy allocator.

### 4.5 Shrinking

`kmem_cache_shrink()` follows the assignment's growth rule:

- if the cache grew after the preceding shrink attempt, the growth flag is
  cleared and no slab is released;
- otherwise, all empty slabs are released and the number of released buddy
  blocks is returned.

### 4.6 Destroying a cache

A cache can be destroyed only when it has no partial or full slabs. Destroying
a busy cache leaves it registered and records `CACHE_BUSY`.

For a destroyable cache, all empty slabs are released, the cache is removed
from the global list, and its descriptor page is returned to the buddy
allocator.

### 4.7 Cache diagnostics

`kmem_cache_info()` prints:

- cache name;
- requested object size;
- total number of allocated buddy blocks;
- slab count;
- objects per slab;
- percentage occupancy.

`kmem_cache_error()` prints and returns one of:

- no error;
- bad argument;
- out of memory;
- invalid object;
- cache busy.

## 5. Small-Buffer Allocation

`kmalloc()` supports requests from 1 byte through 131072 bytes.

Requests are rounded up to these power-of-two size classes:

`32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072`

The corresponding `size-N` cache is created lazily on the first request for
that class.

`kfree()` searches only the size-cache set, not typed kernel-object caches.
This prevents a buffer free from accidentally releasing a process, inode,
file, pipe, or disk-buffer object.

To avoid retaining large amounts of memory after temporary allocation bursts,
size caches retain at most one completely empty slab. Additional empty slabs
are immediately returned to the buddy allocator. This behavior was needed to
preserve xv6's free-page accounting during `usertests`.

## 6. Synchronization

The implementation uses:

- one buddy spinlock for buddy free lists and the block map;
- one global cache-list spinlock for cache registration and size-cache
  creation;
- one spinlock per cache for slab lists, bitmaps, counters, and errors.

The cache list is checked before a cache lock is acquired, preventing calls
through stale or unknown cache handles from modifying allocator state.

Lazy size-cache creation is race-safe. If two callers create the same class,
one cache is registered and the redundant empty cache is destroyed.

## 7. Kernel Integration

### `kernel/kalloc.c`

The old physical-page free list was removed. `kinit()` now initializes the
buddy/slab subsystem over the memory between the kernel image and `PHYSTOP`.

### `kernel/main.c`

Allocator initialization was moved to the beginning of the boot sequence so
that later subsystem initialization can use `kmalloc()` and object caches.

The pipe cache is initialized explicitly during boot.

### `kernel/proc.c` and `kernel/proc.h`

The static `proc[NPROC]` array was replaced by `NPROC` process objects obtained
from a `proc` cache.

The process table is now a linked list. Each process keeps a stable slot
number so its guarded kernel-stack virtual address remains compatible with the
original `KSTACK(slot)` layout.

Trapframes and page tables remain page allocations and are released with
`kpage_free()`.

### `kernel/file.c` and `kernel/file.h`

The static file table was replaced with a `file` object cache.

A scalar count preserves the original `NFILE` limit. Reference counting is
still protected by the file-table lock, and a file object is returned to its
cache only when its reference count reaches zero.

The static device-switch array was replaced by a `kmalloc()` allocation made
during console initialization.

### `kernel/fs.c`

The in-memory inode array was replaced with an `inode` cache and linked list.
A count preserves the original `NINODE` limit.

The inode table state and superblock storage are allocated with `kmalloc()`
instead of being static composite objects.

### `kernel/bio.c`

The `NBUF` disk buffer objects are created from a `buf` cache during `binit()`.
The existing LRU list and reference-count behavior are preserved.

The buffer-cache state structure is allocated with `kmalloc()`.

### `kernel/pipe.c`

Pipe objects are created from a typed `pipe` cache instead of consuming a full
page per pipe.

The cache is warmed during boot so its first slab is included in baseline
memory usage rather than appearing as a leak during later xv6 tests.

### `kernel/console.c`

The console state, 128-byte input ring, 32-byte write staging buffer, and
device-switch table use `kmalloc()`.

The temporary write staging buffer is freed after every console write.

### `kernel/log.c`

The filesystem log state is allocated with `kmalloc()` during log
initialization.

### `kernel/virtio_disk.c`

The VirtIO driver state and its descriptor bookkeeping arrays are allocated
with `kmalloc()`.

The three DMA queue pages still use page-aligned `kalloc()` allocations.

The `struct buf` field named `disk` was renamed to `disk_owned` to avoid a
preprocessor-name collision with the dynamically allocated driver-state
accessor.

### `kernel/sysfile.c`

Temporary `exec` argument buffers use the 4096-byte size cache and are released
with the small-buffer `kfree()` API.

### `kernel/vm.c`

All page-table and user-page releases use `kpage_free()`, while allocation
continues through the buddy-backed `kalloc()`.

## 8. System-Call Interface

System call numbers 22 through 31 were added for:

- `kmem_init`
- `kmem_cache_create`
- `kmem_cache_shrink`
- `kmem_cache_alloc`
- `kmem_cache_free`
- `kmalloc`
- `kfree`
- `kmem_cache_destroy`
- `kmem_cache_info`
- `kmem_cache_error`

The dispatch table is defined in `kernel/syscall.c`, and user stubs are
generated by `user/usys.pl`. `user/user.h` includes the allocator declarations.

### Current syscall assumptions and limitations

The kernel allocator is initialized during boot. The `kmem_init` syscall is
therefore an idempotent no-op and does not replace the live kernel arena.

Cache and allocation values returned through the system calls are opaque
kernel handles. They can be returned to allocator system calls, but user mode
cannot safely dereference them because kernel physical memory is not mapped
into user page tables.

User function pointers cannot safely be executed in supervisor mode.
Consequently, the syscall wrapper rejects non-null constructor or destructor
arguments. Constructors and destructors are fully supported for direct
kernel-mode callers of `kmem_cache_create()`.

These two restrictions are important when adapting the faculty public
allocator test. A public test that writes directly through returned pointers
or passes user-space constructor functions requires an additional test
bridge, shared mapping, or a kernel-side callback mechanism.

## 9. Build-System and Host Compatibility Changes

### `Makefile`

- Added `kernel/slab.o` and `kernel/sysmem.o`.
- Added `-mabi=lp64` to C and assembly compilation.
- Added `-m elf64lriscv` to linker flags.

The explicit ABI and linker emulation are needed by toolchains whose default
target is RV32 even when `-march=rv64gc` is supplied.

### `mkfs/mkfs.c`

The host filesystem-image builder was made portable to Windows:

- BSD `bzero`, `bcopy`, and `index` calls were replaced by standard
  `memset`, `memmove`, and `strchr`;
- input and output files are opened with `O_BINARY` where supported, avoiding
  text-mode translation and invalid reads when processing RISC-V binaries.

## 10. Validation Performed

### Cross-compilation

A clean RV64 build was performed with xPack GNU RISC-V Embedded GCC 15.2.0:

```text
make clean
make TOOLPREFIX=<riscv-none-elf-prefix> kernel/kernel fs.img
```

The kernel, all standard xv6 user programs, and the filesystem image built
successfully.

### Boot test

The clean-built kernel was booted with QEMU `qemu-system-riscv64`. The shell
started and executed:

```text
echo final-boot-ok
```

The expected output was produced without a kernel panic.

### Standard xv6 tests

`usertests -q` completed with:

```text
ALL TESTS PASSED
```

This covers the quick filesystem, process, fork/exec, pipe, page-allocation,
lazy-allocation, invalid-address, and memory-pressure tests.

The full `usertests` run completed all quick tests and these slow tests:

- `bigdir`
- `manywrites`
- `badwrite`
- `execout`
- `diskfull`

The external 10-minute harness timeout occurred while the final
`outofinodes` test was running. That final test was then run separately with:

```text
usertests outofinodes
```

and completed with:

```text
ALL TESTS PASSED
```

Therefore, every standard xv6 `usertests` test was executed and passed, but
not all slow tests completed in one uninterrupted invocation because of the
external timeout.

### Allocator syscall test

A temporary user program was built and run to verify:

- cache creation;
- 200 object allocations across multiple slabs;
- object frees;
- cache information output;
- the two-step shrink rule;
- cache destruction;
- small-buffer size-class boundaries;
- the 131072-byte maximum;
- rejection of a 131073-byte request.

It completed with:

```text
SLAB TEST PASSED
```

The temporary test source and binary were removed afterward and are not part
of the project.

## 11. Tests Not Run

The faculty-provided public allocator application mentioned in the assignment
was not present in the supplied directory and was therefore not run.

No secret grading tests or performance-ranking tests were available.

The allocator was tested with the project default of one CPU. The locking
paths were implemented for concurrent callers, but a multi-CPU stress test was
not run.

