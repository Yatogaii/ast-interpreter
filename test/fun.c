#include <stdio.h>
#include <stdlib.h>

void PRINT(int t) {
    printf("%d", t);
}

void *MALLOC(int t) {
    return malloc(t);
}

void FREE(void *t) {
    free(t);
}
