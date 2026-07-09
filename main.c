#include "fmalloc.h"
#include <stddef.h>
#include <stdio.h>


int main(void){

    int* ptr = fmalloc(sizeof(int) * 10);
    for(int i = 0; i < 10; i++) {
        ptr[i] = (i + 1) * 10;
        printf("%d ", ptr[i]);
    }
    printf("\n");

    ffree(ptr);


    // caching on arenas test
    char *e = (char*)fmalloc(32);
    char *f = (char*)fmalloc(32);
    char *g = (char*)fmalloc(32);

    ffree(e);
    ffree(f);
    ffree(g);

    char *i = (char*)fmalloc(32);
    char *j = (char*)fmalloc(32);
    char *k = (char*)fmalloc(32);

    // large cache test
    char *l = (char*)fmalloc(7000);
    char *m = (char*)fmalloc(7000);
    char *n = (char*)fmalloc(7000);

    ffree(l);
    ffree(m);
    ffree(n);

    char *o = (char*)fmalloc(7400);
    char *p = (char*)fmalloc(7400);
    char *q = (char*)fmalloc(7400);

    // heap growth test
    fmalloc(8000);
    fmalloc(8000);
    fmalloc(8000);
    fmalloc(8000);
    fmalloc(8000);

    // dedicated arena test
    fmalloc(128*1024);

    // frealloc test
    char *r = (char*)fmalloc(400);
    char *s = (char*)frealloc(r, 600);

    print_arenas();

    return 0;
}
