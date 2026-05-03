#define SJ5_IMPL
#include "sj5.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_value(sj5_Reader *r, sj5_Value val, int indent);

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) printf("  ");
}

static void print_object(sj5_Reader *r, sj5_Value obj, int indent) {
    sj5_Value key, val;
    printf("{\n");
    while (sj5_iter_object(r, obj, &key, &val)) {
        print_indent(indent + 1);
        printf("%.*s: ", (int)(key.end - key.start), key.start);
        print_value(r, val, indent + 1);
        printf(",\n");
    }
    print_indent(indent);
    printf("}");
}

static void print_array(sj5_Reader *r, sj5_Value arr, int indent) {
    sj5_Value val;
    printf("[\n");
    while (sj5_iter_array(r, arr, &val)) {
        print_indent(indent + 1);
        print_value(r, val, indent + 1);
        printf("\n");
    }
    print_indent(indent);
    printf("]");
}

static void print_value(sj5_Reader *r, sj5_Value val, int indent) {
    switch (val.type) {
    case SJ5_OBJECT: print_object(r, val, indent); break;
    case SJ5_ARRAY:  print_array(r, val, indent);  break;
    case SJ5_STRING: printf("\"%.*s\"", (int)(val.end - val.start), val.start); break;
    case SJ5_NUMBER:
    case SJ5_BOOL:
    case SJ5_NULL:   printf("%.*s", (int)(val.end - val.start), val.start); break;
    case SJ5_ERROR:  printf("<error>"); break;
    }
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "example.json5";
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = malloc(len);
    fread(buf, 1, len, f);
    fclose(f);

    sj5_Reader r = sj5_reader(buf, len);
    sj5_Value root = sj5_read(&r);
    print_value(&r, root, 0);
    printf("\n");

    if (r.error) {
        int line, col;
        sj5_location(&r, &line, &col);
        fprintf(stderr, "error at %d:%d: %s\n", line, col, r.error);
        free(buf);
        return 1;
    }
    free(buf);
    return 0;
}
