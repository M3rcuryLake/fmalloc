# fmalloc, A Dynamic Memory Allocater.

`fmalloc` is a custom dynamic memory allocator implemented as a educational replacement (not really, don't use this in your production environment) for the standard GLIBC `malloc`. The code is highly influenced by the glibc malloc as I wanted to study the internals of the system. The allocator obtains memory from the operating system using sbrk() for allocating or growing the main arena and mmap() for larger, non-main or separate mappings, with experimental support for extending mmap() backed arenas using mremap().

At its core, `fmalloc` is a simple arena-based allocator with a tcache-like reuse layer. Fresh allocations are carved from an arena using a moving `top` pointer. The allocator uses bump-pointer allocation only on cache miss. On free, chunks are not discarded or reset arena-wide; they are inserted into size-indexed cache bins and may be reused later. 

## Public API

```c
void *fmalloc(size_t size);
void *fcalloc(size_t n, size_t size);
void *frealloc(void *ptr, size_t size);
void *ffree(void *ptr);

void print_arenas(void);
```

These mirror the standard allocation API:

* `fmalloc(size)` allocates `size` bytes.
* `fcalloc(n, size)` allocates and zeroes `n * size` bytes.
* `frealloc(ptr, size)` resizes an allocation.
* `ffree(ptr)` frees an allocation.

`print_arenas(void)` can be used to examine the arenas on the fly.


## Features

* Arena-based heap management
* Bump-pointer allocation inside arenas
* `sbrk()` backed main heap allocation
* `mmap()` backed arena allocation
* `mremap()` and `sbrk()` based arena growth
* Compact chunk headers using size-and-flag packing
* Tcache like freed-chunk reuse called `cache`
* Tcache like overlay for free-list metadata
* Glibc like safe-linking pointer mangling for cache `next` pointers
* Basic double-free detection using a per-process cache key
* Single lock thread safety using a global allocator lock
* Debug arena printing
* Corruption trap support using `__builtin_trap()`

## Design Overview

The allocator manages memory using arenas. Each arena describes one contiguous memory region:

```c
typedef struct arena {
    size_t size;
    uint8_t *base;
    uint8_t *top;
    struct arena *next;
    struct arena *prev;
    uint8_t *end;
} arena;
```

`base` points to the first usable byte after the arena header. `top` points to the next free position for bump allocation. `end` marks the end of the arena. Arenas are connected through a doubly linked list, allowing the allocator to manage multiple memory regions.

Each allocation has a small chunk header:

```c
typedef struct chunk {
    size_t chunk_size;
} chunk;
```

The `chunk_size` field stores both the real chunk size and low-bit flags such as `CHUNK_INUSE`. Since chunks are aligned, the lower bits of the size field are available for metadata flags.

When a chunk is freed, its user payload is reused as cache metadata:

```c
typedef struct cache_struct {
    struct cache_struct *next;
    uintptr_t key;
} cache_struct;
```

This is the same general idea used by tcache style allocators: allocated chunks expose payload to the user, while freed chunks reuse that payload space to link themselves into free lists.

## Allocation Flow

The simplified allocation path is:

```text
fmalloc(size)
    -> normalize request with request2size()
    -> map normalized size to cache bin with chunk2idx()
    -> if cache bin has a free chunk, reuse it
    -> otherwise allocate from arena->top
    -> if no arena has space, grow/create an arena
```

Fresh chunks are allocated using bump-pointer logic:

```c
chunk *n = (chunk *)a->top;
a->top = a->top + size;
n->chunk_size = size | CHUNK_INUSE;
```

This makes fresh allocation simple and fast. The tradeoff is that the allocator currently does not coalesce or split chunks.

## Free Flow

The simplified free path is:

```text
ffree(ptr)
    -> convert user pointer back to chunk
    -> validate chunk metadata
    -> map chunk size to cache bin
    -> store freed chunk in cache bin
```

Freed chunks are placed into cache bins:

```c
e->key = cache_key;
e->next = PROTECT_PTR(&e->next, cache.entries[idx]);
cache.entries[idx] = e;
cache.counts[idx]++;
```

The cache key along with `CHUNK_INUSE` is used as a simple double-free detection mechanism. The `next` pointer is protected using safe-linking inspired from glibc malloc.

## Allocator Internals

Though I have tried to emulate the essence and intrinsic nature of the `malloc` code to an extent, there were places where it would have been simply too hard for me, such as tcache-large-bins. Just to implement a simple idea I would have had to implement Unsorted Bin, Chunk Splitting etc, which would have vastly increased the size of the code. Well, too much for a one-week project, right?

So yeah, the allocator uses two broad size-class regions:

```text
small bins:
    exact 16-byte aligned classes

large bins:
    bucketed classes, currently based around 2048-byte multiples
```

`request2size()` converts a user request into an internal chunk size by adding the chunk header, enforcing a minimum payload size, aligning the result, and optionally rounding larger chunks into wider buckets. While `chunk2idx()` maps the normalized chunk size to a cache-bin index. This means two requests that normalize to the same size class can reuse chunks from the same cache bin.

In this implementation, an allocated chunk looks the same as free chunk externally as there is nothing added to the stuct area after the chunk is freed (the cache bin data such as next and key is overlaid on the user data after free). So the chunk looks like this :

```
allocated chunk
┌──────────────────────────────┬──────────────────────────────────────┐
│ chunk header                 │ user payload                         │
│                              │                                      │
│ chunk_size                   │ returned pointer points here         │
│ size | CHUNK_INUSE           │                                      │
└──────────────────────────────┴──────────────────────────────────────┘
^                              ^
chunk *c                       user pointer (or) chunk2mem(c)
```
```
freed chunk
┌───────────────┬───────────────┬──────────┬─────────┬──────────────┐
│ chunk_size    │ padding       │   next   │   key   │	user data   |
│ size + flags  │ to 16 bytes   │          │         │              |
└───────────────┴───────────────┴──────────┴─────────┴──────────────┘
^                               ^
chunk *c                        cache_struct *e = chunk2mem(c)
```

Cache bin list : One cache bin is a singly linked list of freed chunks of the same size class. This is the same as tcache_perthread_struct in the glibc malloc code. To get more idea of this, just look at the code in malloc. The diagram would look like as follows :

```
cache.entries[idx]
       |
       ▼
┌────────────────────┐      ┌────────────────────┐      ┌────────────────────┐
│ freed chunk A      │      │ freed chunk B      │      │ freed chunk C      │
│                    │      │                    │      │                    │
│ [chunk_size]       │      │ [chunk_size]       │      │ [chunk_size]       │
│ [next protected]───┼─────>│ [next protected]───┼─────>│ [next protected]───┼──> NULL
│ [key]              │      │ [key]              │      │ [key]              │
└────────────────────┘      └────────────────────┘      └────────────────────┘
```

Arenas in glibc malloc are typedef-ed as mstate. In this code, the arena layout looks like this :
arena mapping / heap region
```
┌──────────────────────────┬───────────────────────┐
│       SIZE + flags       │     BASE POINTER      │                          
│--------------------------│-----------------------│                                                                      
│      TOP_CHUNK_PTR       │     NEXT_ARENA_PTR    │
│--------------------------│-----------------------│                       
│    PREVIOUS_ARENA_PTR    │        ARENA_END      │                                    
└──────────────────────────┴───────────────────────┘

head_ar_ptr
   │
   ▼
┌──────────────┐      ┌──────────────┐      ┌──────────────┐
│ main arena   │─────>│ mmap arena   │─────>│ sbrk arena   │─────> NULL
│ prev = NULL  │<─────│ prev = main  │<─────│ prev = mmap  │
└──────────────┘      └──────────────┘      └──────────────┘
```

## Usage

Just move `fmalloc.c` and `makefile` to your project directory and run the following. If you just want to check my code
```bash
make SRC=test.c OUT=test
# this does test.c + fmalloc.c -> test

./test

# If you just want to check my code
make
./main
```
If you want to check the `arenas` and the `cache bins` in your code, just call the `print_arenas()` function, it will cause a SIGILL (Illegal intstuction error) to quit the execution once encountered, but will print all the arenas in an orderly fashion. If still not happy, I suggest using `pwndbg` and examining `current_ar_ptr` and `cache` manually. 


## Current Limitations

`fmalloc` is experimental and intentionally incomplete. Current limitations include:

* No chunk splitting
* No coalescing of adjacent free chunks
* No boundary tags
*  No fence-post chunks
* No `prev_size`
* No best-fit or first-fit free-list allocator
* Currently, caching a chunk to freelist wastes 8 bytes of memory. 
* Single global lock for an API call instead of scalable per-thread caches
* `realloc()` currently only remaps to a larger chunk (no splitting)

The allocator is therefore best described as a bump-pointer arena allocator with tcache-like free reuse and mmap/sbrk backed arenas with support for arena growth.

## Project Goal

Nothing really, It's just that I wanted to study malloc internals. Props to [pwn.college](https://pwn.college/) and [Axura's Blogpost](https://4xura.com/binex/glibc/ptmalloc2/ptmalloc-the-gnu-allocator-a-deep-gothrough-on-how-malloc-free-works/)

## License
This project is licensed under the [MIT](https://choosealicense.com/licenses/mit/) License - see the [LICENSE.md](https://github.com/m3rcurylake/fmalloc/LICENSE.md) file for details.
