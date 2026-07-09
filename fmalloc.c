#define _GNU_SOURCE

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/mman.h>
#include <pthread.h>
#include <sys/random.h>

/* Only for devlopment, do not enable on prod.
 * #define DEBUG
 * #define HALT_ON_CORRUPTION
 */

#define IS_MMAPPED 0x1
#define DEDICATED_ARENA  0x2   // set for both chunk and arenas

#define CHUNK_INUSE 0x4

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t fmalloc_lock = PTHREAD_MUTEX_INITIALIZER;

/* Safe-Linking for Dummies */
#define PROTECT_PTR(pos, ptr) \
  ((typeof (ptr)) ((((size_t) pos) >> 12) ^ ((size_t) ptr)))
#define REVEAL_PTR(ptr)  PROTECT_PTR (&ptr, ptr)

/*
 * Heap Constants, Defines basic global constants for the allocator
 * and is pretty similar to Glibc.
 */
#define HEAP_MIN_SIZE (32 * 1024)           // 32Kb
#define HEAP_MAX_SIZE (1024 * 1024)         // 1MB

#define MAX_SMALL_BINS 64                         // ranged from 32B to 1040B, with intervals of 16 Bytes
#define SMALL_BIN_SIZE (MINSIZE + (MAX_SMALL_BINS - 1) * MALLOC_ALIGNMENT)  // covers from 0 to 1023 B
#define BIN_CHUNK_COUNT 14

#define MAX_LARGE_BINS 16
#define LARGE_BIN_STEP 2048


#define MAX_BIN_SIZE (32*1024)
#define MAX_BINS (MAX_SMALL_BINS + MAX_LARGE_BINS)

/* Alignment definations */
#define SIZE_SZ (sizeof(size_t))
#define MALLOC_ALIGNMENT (2 * SIZE_SZ)
#define MALLOC_ALIGN_MASK (MALLOC_ALIGNMENT - 1)

#define LARGE_BIN_MASK (LARGE_BIN_STEP-1)
#define LARGE_BIN_SHIFT    11


#define PAGE (sysconf(_SC_PAGESIZE))
#define PAGE_MASK (PAGE-1)

/* The smallest size we can malloc is an aligned minimal chunk, 32 Bytes */
#define MINSIZE  0x20

static inline size_t alloc_align(size_t size, size_t mask){
    return (size + mask) & ~mask;
}

static inline size_t page_align(size_t size){
    /* _SC_PAGESIZE is a constant passed to sysconf() that returns page size
     * The page size matters because mmap() works in units of pages. Even if you ask for 5 bytes
     * the kernel still maps at least one full page internally, usually 4096 bytes.
     * So this inline function rounds size up to the nearest page boundary. */
    return (size + PAGE_MASK) & ~PAGE_MASK;
}

static inline size_t get_real_size(size_t size){
    return (size) & ~0x0f;
}

static inline size_t set_is_mmaped(size_t size){
    return size | IS_MMAPPED ;
}

static inline size_t set_chunk_inuse(size_t size){
    return size | CHUNK_INUSE ;
}


typedef struct chunk {
    // currently wasting 8 bytes to match alignmengt
    size_t chunk_size;
} chunk;

typedef struct cache_struct {
    // completly same as tcache_entry
    struct cache_struct *next;
    uintptr_t key;
} cache_struct;

typedef struct cache_bin {
    // completely same as tcache_perthread_struct
    cache_struct *entries[MAX_BINS];
    uint16_t counts[MAX_BINS];
} cache_bin;

typedef struct arena {
    size_t size;            // size & flags
    uint8_t *base;          // usable data start
    uint8_t *top;           // top chunk
    struct arena *next;     // next arena
    struct arena *prev;     // prev arena
    uint8_t *end;           // (void*) a + size
} arena;

arena *head_ar_ptr = NULL;      // first arena in the arena chain
arena *current_ar_ptr = NULL;   // arena currently used
static cache_bin cache;         // tcache but NOT tcache ;)
static uintptr_t cache_key;     // double free check

static inline int8_t ar_check_if_dedicated(arena *a){
    return (a->size & DEDICATED_ARENA)!=0;      // rets true if yes
}

static inline int8_t ch_check_if_dedicated(chunk *a){
    return (a->chunk_size & DEDICATED_ARENA)!=0;      // rets true if yes
}

static inline int8_t check_is_mmaped(arena *a){
    return (a->size & IS_MMAPPED)!=0;           // rets true if yes
}

static inline size_t chunk_hdr_size(void){
    return alloc_align(sizeof(chunk), MALLOC_ALIGN_MASK);
}

static inline size_t arena_hdr_size(void){
    return alloc_align(sizeof(arena), MALLOC_ALIGN_MASK);
}

static size_t request2size(size_t req){
/* pad request bytes into a usable size (GLIBC)
 * In my implementation chunk header is 8 bytes and a freed chunk must fit a cache_struct,
 * the minimum payload must be at least sizeof(cache_struct).
 */
  if (req < sizeof(cache_struct)){
    req = sizeof(cache_struct);
  }
  if (req > HEAP_MAX_SIZE){
      return 0;
  }

  size_t total;
  if (__builtin_add_overflow(chunk_hdr_size(), req, &total)) {
      return 0;
  }

  if (total < MINSIZE){
      total = MINSIZE;
  }

  total = alloc_align(total, MALLOC_ALIGN_MASK);

   // sizes from 2048 to 32*1024 with step of 2048
   if (total >= SMALL_BIN_SIZE) {
       return (total + LARGE_BIN_STEP - 1) & ~(LARGE_BIN_STEP - 1);
   }

   // sizes from 32 to 1024 with step of 16
   return total;
}

static int8_t chunk2idx(size_t size){
    if (size < MINSIZE || size > MAX_BIN_SIZE){
        return -1;      // Bounds Check
    }
    if (size <= SMALL_BIN_SIZE) {
        if ((size & MALLOC_ALIGN_MASK) != 0) {
            return -1;
        }

        return (int8_t)((size - MINSIZE) / MALLOC_ALIGNMENT);
    }

    /* we get the following mapping for chunks
     *
     *  size 0x20  => idx  0
     *  size 0x30  => idx  1
     *  size 0x40  => idx  2
     *  (...)
     *
     *  size 2KiB  => idx  SMALL_BINS + 0
     *  size 4KiB  => idx  SMALL_BINS + 1
     *  size 6KiB  => idx  SMALL_BINS + 2
     *  size ...
     *  size 32768 -> SMALL_BINS + 15
     */

    if ((size & (LARGE_BIN_STEP - 1)) != 0) {
        return -1;
    }

    return (int8_t)(MAX_SMALL_BINS + ((size >> LARGE_BIN_SHIFT) - 1));
}


static inline void *chunk2mem(chunk *c){
    return (void *)((uint8_t *)c + chunk_hdr_size());
}

static inline chunk *mem2chunk(void *p){
    return (chunk *)((uint8_t *)p - chunk_hdr_size());
}

void print_arenas(void){
    char *arena_type;

    if (head_ar_ptr!=NULL){
        printf("\n\n [+] Arenas : \n");
        printf("┌──────────────┬────────────────────┬────────────────────┬────────────────────┬────────────────────┬────────────────────┬────────────┐\n");
        printf("│ %-12s │ %-18s │ %-18s │ %-18s │ %-18s │ %-18s │ %-10s │\n", "Arena Type", "Arena Address", "Top Chunk", "Next Arena", "Prev Arena", "Arena End", "Size");
        printf("├──────────────┼────────────────────┼────────────────────┼────────────────────┼────────────────────┼────────────────────┼────────────┤\n");

        for(arena *a = head_ar_ptr; a != NULL; a = a->next){
            if (a == head_ar_ptr) {
                arena_type = "main_arena";
            } else {
                arena_type = "arena";
            }

            printf("│ %-12s │ %-18p │ %-18p │ %-18p │ %-18p │ %-18p │ %-10zu │\n", arena_type, a, a->top, a->next, a->prev, a->end, get_real_size(a->size));
        }

        printf("└──────────────┴────────────────────┴────────────────────┴────────────────────┴────────────────────┴────────────────────┴────────────┘\n");

    } else {
        printf("Head Arena is NULL.");
    }
}


static void alloc_printerr(const char* func, const char* message){

    char buf[128];
    sprintf(buf, "(%s) : %s", func, message);
    fprintf(stderr, "%s", buf);

    #ifdef DEBUG
    {
        print_arenas();

        #ifdef HALT_ON_CORRUPTION
        {
            __builtin_trap();
        }

        #endif
    }
    #endif
}


static void *__mmapalloc(size_t size){
  size_t aligned_size = ((size+MALLOC_ALIGN_MASK)&~MALLOC_ALIGN_MASK);
  if (aligned_size == 0){
      return NULL;
  }
  void *chunk = mmap(NULL, aligned_size,
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS,
                        -1, 0);
  if (chunk == MAP_FAILED){
    alloc_printerr(__func__, "mmap() failed to allocate.\n");
    return MAP_FAILED;
  }
  return chunk;
}


static void *__sbrkalloc(size_t size){

if (size==0) return NULL;

  size_t aligned_size = ((size+MALLOC_ALIGN_MASK)&~MALLOC_ALIGN_MASK);
  pthread_mutex_lock(&lock);
  void *requestChunk = sbrk(aligned_size);

  if (requestChunk == MAP_FAILED){
    pthread_mutex_unlock(&lock);
    alloc_printerr(__func__, "sbrk() failed to allocate.\n");
    return NULL;
  }

pthread_mutex_unlock(&lock);
return requestChunk;
}

static int __mmapdealloc(void * ptr, size_t size){
    if (size == 0 && size % PAGE != 0) return -1;

    int dealloc = munmap(ptr, size);
    if (dealloc == -1){
        alloc_printerr(__func__, "munmap() deallocation failed.\n");
        return -1;
    }
    return dealloc;
}

static int __sbrkdealloc(size_t size){
    if (size==0) return -1;
    pthread_mutex_lock(&lock);
    void *requestChunk = sbrk(-size);
    if (requestChunk == (void *) -1){
        pthread_mutex_unlock(&lock);

        alloc_printerr(__func__, "sbrk() failed to deallocate.\n");
        return -1;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

static int __mremap_extend(void * ptr, size_t size, size_t extension){

    if (size == 0){
        return -1;
    }

    extension = page_align(extension);
    size_t remaped_to_size =  size + extension;
    void *remap = mremap(ptr, size, remaped_to_size, 0);

    if (remap == MAP_FAILED){
        alloc_printerr(__func__, "mremap() failed to allocate.\n");
        return -1;
    }
    return 0;
}

#define MMAP_ALLOC_THRESHOLD (HEAP_MIN_SIZE + arena_hdr_size() + 1)

static arena *create_arena(size_t min){
    size_t arena_size = HEAP_MIN_SIZE;
    if (min > arena_size){
        arena_size = min;
    } else {
        min = arena_size;
    }
    size_t arena_header_size = arena_hdr_size();
    size_t total_size = arena_header_size + arena_size;

    uint8_t is_mmaped = 0;
    void *mem = NULL;

    // if less than mmap threshold then brk or else mmap, if brk fails then mmap
    if (total_size < MMAP_ALLOC_THRESHOLD){
        mem = __sbrkalloc(total_size);
        if (mem == NULL){
            total_size = page_align(total_size);
            mem = __mmapalloc(total_size);
            if (mem == MAP_FAILED || mem == NULL){
                return NULL;
            }
            is_mmaped = 1;
        }
    } else {
        total_size = page_align(total_size);
        mem = __mmapalloc(total_size);
        if (mem == MAP_FAILED || mem == NULL){
            return NULL;
        }
        is_mmaped = 1;
    }
    arena *a = (arena *)mem;


    a->size = (is_mmaped) ? set_is_mmaped(total_size) : total_size;
    a->base = (uint8_t *)mem + arena_header_size;
    a->top = a->base;
    a->next = NULL;
    a->prev = NULL;
    a->end = (uint8_t *)mem + total_size;

    return a;
}

static int8_t grow_arena(arena * a, size_t size){
    // Increase the heap size and create a new chunk to fulfill the request
    // until the MAX_ARENA_SIZE is reached.
    // when grow_arena() returns -1, create a new arena.

    if (a==NULL || size == 0) return -1;

    size = page_align(size);
    size_t ar_size = get_real_size(a->size);

    if (size > MMAP_ALLOC_THRESHOLD){
        return -1;
    }

    int8_t flags = a->size & 0xF;
    if (ar_size + size > HEAP_MAX_SIZE) {
        return -1;
    }

    if (a == head_ar_ptr){

        if (!check_is_mmaped(a)){
            void *p = __sbrkalloc(size);

            if (p == NULL) {
                return -1;
            }

            a->size = (ar_size + size) | flags;
            a->end += size;

            return 0;

        } if (check_is_mmaped(a)) {
            if (__mremap_extend((void *)a, ar_size, size) != 0) {
                return -1;
            }
            a->size = (ar_size + size) | flags;
            a->end = (uint8_t *)a + size;
        }
    }

    if (a != head_ar_ptr){
        chunk *n = (chunk*)a->base;

        if (ch_check_if_dedicated(n)){
            return -1;
        } else {
            if (!check_is_mmaped(a)){
                void *p = __sbrkalloc(size);

                if (p == NULL) {
                    return -1;
                }

                a->size = (ar_size + size) | flags;
                a->end += size;

                return 0;

            } else {
                if (__mremap_extend((void *)a, ar_size, size) != 0) {
                    return -1;
                }

                a->size = (ar_size + size) | flags;
                a->end = (uint8_t *)a + size;

            }
        }
    }

    return 0;
}

static int arena_init(size_t min){
    if (current_ar_ptr == NULL && min < MMAP_ALLOC_THRESHOLD){
        // if there is no arena in the first place then create one.
        arena *a = create_arena(min);
        if (a == NULL) return -1;

        head_ar_ptr = a;
        current_ar_ptr = a;
    }
    if (current_ar_ptr == NULL && min > MMAP_ALLOC_THRESHOLD){
        // if the first malloc is greater than MMAP_ALLOC_THRESHOLD, then first
        // create a main_arena and then proceed to create a dedicated arena.
        arena *a = create_arena(HEAP_MIN_SIZE);
        if (a == NULL) return -1;

        head_ar_ptr = a;
        current_ar_ptr = a;
        arena *sa_addr = current_ar_ptr;

        arena *an = create_arena(min);

        if (an == NULL) return -1;

        current_ar_ptr->next = an;
        current_ar_ptr = an;
        current_ar_ptr->prev = sa_addr;
        current_ar_ptr->size = set_is_mmaped(current_ar_ptr->size);
    }

    if ((size_t)(current_ar_ptr->end - current_ar_ptr->top) < min){
        arena *sa_addr = current_ar_ptr;

        // if there is no space in the current arena, the attempt to grow it, if fails
        // then create a new non-dedicated arena.

        int8_t g = grow_arena(current_ar_ptr, min);

        if (g == 0) {
            return 0;
        }

        arena *a = create_arena(min);
        if (a == NULL) {
            return -1;
        }

        current_ar_ptr->next = a;
        current_ar_ptr = a;
        current_ar_ptr->prev = sa_addr;
    }

    return 0;
}

static arena *get_arena(size_t size){
    // Given that size is alloc_aligned already
    // support to search multiple arenas for free space to allocate chunk

    if (size==0) return NULL;

    for(arena *a = head_ar_ptr; a != NULL; a = a->next){
        if (!ar_check_if_dedicated(a)){
            size_t u_data = (size_t)(a->end - a->top);
            if (u_data>=size){
                return a;
            }
        }
    }

    return NULL;
}

static int destroy_mmap_arena(arena *a){
    if (a == NULL) return -1;

    if (a == head_ar_ptr) return -1;

    size_t ar_size = a->size;
    size_t size = get_real_size(ar_size);
    uint8_t *arena = (uint8_t * ) a;

    if ((ar_size & 0xf) == 0x1 || (ar_size & 0xf) == 0x2 ||(ar_size & 0xf) == 0x3 ){

        if (a->prev != NULL){
            // this sets the next section of the arena previous to the arena we are
            // destroying to point to the arena after the arena we are destoying.
            a->prev->next = a->next;
        } else {
            // if a->prev == NULL then it is the first arena (head_ar_ptr)
            head_ar_ptr->next = a->next;
        }

        if (a->next != NULL){
            // this sets the prev section of the arena next to the arena we are
            // destroying to point to the arena before the arena we are destoying.
            a->next->prev = a->prev;
        } else {
            // if a->next == NULL, then it is the last arena (current_ar_ptr)
            current_ar_ptr->prev = (a->prev)? a->prev : head_ar_ptr;
        }

        int ret = __mmapdealloc(arena, size);
        if (ret != 0){
            return -1;
        }

        return ret;
    }

    alloc_printerr(__func__, "Error encountered when destroying mmap()-ed arena.\n");
    return -1;
}

static chunk *alloc_chunk(size_t size){
    // "size" must be a chunk size returned by request2size(req).
    // this assumes size = u_data + header, is aligned to 16 Bytes and is not larger than 1Kib

    arena *a = get_arena(size);

    if (a == NULL) {
        if (arena_init(size)!=0){
            return NULL;
        }

        a = get_arena(size);
        if (!a) return NULL;
    }

    chunk *n = (chunk *)a->top;
    a->top = a->top + size;      // updates the top here

    size = set_chunk_inuse(size);
    n->chunk_size = size;
    return n;
}


static void cache_key_init(void){
    ssize_t __randomGet =  getrandom(&cache_key, sizeof(cache_key), GRND_NONBLOCK);
    if (__randomGet != sizeof(cache_key) || cache_key != 0){
        return;
    }

    cache_key ^= (uintptr_t)time(NULL);
}

// make sure that the cache_key_init is called only once
static pthread_once_t once_control = PTHREAD_ONCE_INIT;

static void pthread_cache_key_init(void){
    pthread_once(&once_control, cache_key_init);
}



static int free_chunk(chunk* ptr){

    if (ptr == NULL) return -1;
    size_t size = get_real_size(ptr->chunk_size);

    int8_t idx = chunk2idx(size);

    if (idx == -1) return -1;

    cache_struct *e = (cache_struct * )chunk2mem(ptr);

    if ((ptr->chunk_size & CHUNK_INUSE) == 0) {
        alloc_printerr(__func__, "Double free or corrupted chunk (CHUNK_INUSE) (1).\n");
        return -1;
    }

    if (e->key == cache_key) {
        alloc_printerr(__func__, "Double free or corrupted chunk (bad key) (2).\n");
        return -1;
    }
    if (((uintptr_t)e & 0xf) != 0) {
        alloc_printerr(__func__, "Unaligned cache chunk detected.\n");
        return -1;
    }

    if (cache.counts[idx] >= BIN_CHUNK_COUNT){
        return -1;  // beyond the scope of allowed bins in cache bins
    }

    ptr->chunk_size = size;

    e->key = cache_key;
    e->next = PROTECT_PTR(&e->next, cache.entries[idx]);

    cache.entries[idx] = e;
    ++(cache.counts[idx]);

    return 0;
}


static void *int_malloc(size_t size){

    size_t nb = request2size(size);

    if (nb == 0 ) {
        alloc_printerr(__func__, "Cannot allocate a chunk of size 0.\n");
        return NULL;
    } if (nb == 0 ) {
        alloc_printerr(__func__, "Cannot allocate a chunk larger than HEAP_MAX_SIZE.\n");
        return NULL;
    }

    int8_t idx =  chunk2idx(nb);
    if (idx != -1 && cache.counts[idx] > 0){

        // Reuse the allocation in front of the list if available
        cache_struct *e = cache.entries[idx];
        cache.entries[idx] = REVEAL_PTR(e->next);
        cache.counts[idx]--;

        // Chunk already allocated, so just repurpose...
        e->key = 0;
        chunk *n = mem2chunk((void *)e);
        n->chunk_size = set_chunk_inuse(get_real_size(n->chunk_size));

        return chunk2mem(n);
    }
    // if idx == -1, then not cacheable. Genrate new chunk from scratch
    chunk *n = alloc_chunk(nb);
    if (n == NULL) return NULL;

    if (nb > MMAP_ALLOC_THRESHOLD){
        n->chunk_size = (n->chunk_size | DEDICATED_ARENA) ;
    }

    return chunk2mem(n);
}

static void int_free(void *ptr){

    if (ptr == NULL) return;
    chunk *n = mem2chunk(ptr);

    size_t size = get_real_size(n->chunk_size);
    if (size < MINSIZE){
        alloc_printerr(__func__, "Got a size less than MINSIZE.\n");
        return;      // size less than min size err
    }


    if (ch_check_if_dedicated(n)){
        // check if the chunk is dedicated, if so then the arena is dedicated too

        arena *a = (arena *)(ptr - arena_hdr_size() - chunk_hdr_size());
        if (check_is_mmaped(a)){
            int rv = destroy_mmap_arena(a);
            if (!rv) return;
        } else {
            return;
        }
    } if (size <= MAX_BIN_SIZE) {
        if(!free_chunk(n)) return;
    }
}

static void *int_realloc(void * ptr, size_t size){

    if (ptr == NULL) {
        return int_malloc(size);
    } if (size == 0) {
        int_free(ptr);
    }

    size_t req_sz = request2size(size);

    chunk *n = mem2chunk(ptr);
    size_t chunk_size = get_real_size(n->chunk_size);

    size_t payload_sz = chunk_size - chunk_hdr_size();

    // Double free check on realloc (?)
    if ((n->chunk_size & CHUNK_INUSE)==0){
        alloc_printerr(__func__, "Cannot reallocate free/corrupted chunk (!CHUNK_INUSE).");
    }

    if (chunk_size >= req_sz){
        // cannot reallocate to a smaller chunk for now
        return ptr;
    }

    void *new_loc = int_malloc(size);
    if (new_loc == NULL){
        return NULL;
    }

    if (payload_sz > size){
        payload_sz = size;
    }

    memcpy(new_loc, ptr, payload_sz);
    int_free(ptr);

    return new_loc;
}


void *fmalloc(size_t size){

    void * f;
    pthread_cache_key_init();

    pthread_mutex_lock(&fmalloc_lock);
    f = int_malloc(size);
    pthread_mutex_unlock(&fmalloc_lock);

    return f;
}

void *fcalloc(size_t n, size_t size){
    /* dynamically allocates memory on the heap and initializes all allocated bytes
     * of the current chunk to zero, and returns a void pointer to the first byte */

    void * f;
    size_t t;

    pthread_cache_key_init();

    if (__builtin_mul_overflow(n, size, &t)){
        alloc_printerr(__func__, "Multiplication of (n, size) overflowed.\n");
        return NULL;
    } else {
        pthread_mutex_lock(&fmalloc_lock);
        f = int_malloc(size * n);
        pthread_mutex_unlock(&fmalloc_lock);

        memset(f, 0, t);

        return f;
    }

    return NULL;
}

void *frealloc(void * ptr, size_t size){
    /* this function changes the size of the memory block pointed to
    by ptr to size bytes. The contents of the memory will be unchanged in
    the  range  from  the start of the region up to the minimum of the old
    and new sizes.
    If ptr is NULL, then the call is equivalent to malloc(size).
    If size is equal to zero, and ptr is not NULL, then the call is equivalent to free(ptr) */


    void * f;
    pthread_cache_key_init();

    pthread_mutex_lock(&fmalloc_lock);
    f = int_realloc(ptr, size);
    pthread_mutex_unlock(&fmalloc_lock);

    return f;
}


void *ffree(void * ptr){

    pthread_cache_key_init();

    pthread_mutex_lock(&fmalloc_lock);
    int_free(ptr);
    pthread_mutex_unlock(&fmalloc_lock);

    return NULL;
}
