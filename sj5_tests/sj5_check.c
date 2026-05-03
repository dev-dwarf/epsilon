#define SJ5_IMPL
#include "sj5.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: sj5_check <file>\n");
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc(len + 1);
    fread(buf, 1, len, f);
    fclose(f);

    sj5_Reader r = sj5_reader((u8*)buf, (size_t)len);

    /* Read the root value — empty document is an error */
    sj5_Value root = sj5_read(&r);
    if (root.type == SJ5_ERROR) {
        fprintf(stderr, "parse error: %s\n", r.error);
        free(buf);
        return 1;
    }
    if (root.type == SJ5_END) {
        fprintf(stderr, "parse error: unexpected end at top level\n");
        free(buf);
        return 1;
    }

    /* For containers, drain all nested tokens until depth returns to 0 */
    if (root.type == SJ5_OBJECT || root.type == SJ5_ARRAY) {
        while (!r.error && r.depth > 0) {
            sj5_read(&r);
        }
    }

    if (r.error) {
        fprintf(stderr, "parse error: %s\n", r.error);
        free(buf);
        return 1;
    }

    /* Check for trailing content after the root value */
    sj5_Value trailing = sj5_read(&r);
    if (trailing.type != SJ5_ERROR || strcmp(r.error, "unexpected eof") != 0) {
        fprintf(stderr, "parse error: trailing content after root value\n");
        free(buf);
        return 1;
    }

    free(buf);
    return 0;
}
