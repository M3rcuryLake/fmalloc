#include <stddef.h>

#define DEBUG

void *fcalloc(size_t nmemb, size_t size);
void *fmalloc(size_t size);
void ffree(void *ptr);
void *frealloc(void *ptr, size_t size);

#ifdef DEBUG
void print_arenas(void);
#endif
