#define LF_IMPL 
#include <lf.h>
#define SJ5_IMPL
#include "sj5.h"

// TODO(lf) should probably move file functions to lf.h
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
str read_file(Arena *a, const char *path) {
  str out; 
  struct stat st;
  int fd = open(path, O_RDONLY);
  if (fstat(fd, &st) == 0) {
    out = str_sized(a, st.st_size);
    out.len = read(fd, out.str, st.st_size);
  }
  close(fd);
  return out;
}

bool write_file(const char *path, u8* data, sptr len) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  sptr n = 0;
  while (n < len) {
    s64 i = write(fd, data + n, len - n);
    if (i > 0) {
      n += i;
    } else {
      break;
    }
  }
  close(fd);
  return n != len;
}
//////////////////////////////////////////////////////////////////

// TODO(lf) should probably move buffered output to lf.h as well
typedef struct Buf Buf;
struct Buf { 
  u8 *buf;
  s32 len;
  s32 cap;
  s32 fd;
  s32 err;
};
void append(Buf *b, u8 *data, s32 len) {
  s32 avail = b->cap - b->len;
  s32 amount = b->err? 0 : CLAMP(len, 0, avail);
  for (s32 i = 0; i < amount; i++) {
    b->buf[b->len+i] = data[i];
  }
  b->len += amount;
  b->err |= amount < len;
}
#define append_strl(b, sl) append(b, (u8*)sl, sizeof(sl"")-1)
void append_str(Buf *b, str s) { append(b, s.str, s.len); }

//////////////////////////////////////////////////////////////////

typedef struct sj5_LL {
  struct sj5_LL *next;
  sj5_Value v;
  sj5_Value k;
} sj5_LL;

#define LL_init(l) sj5_LL *l = 0, **l##_end = &l
#define LL_append(a, l, V, K) do { if (!*l##_end) { \
  *l##_end = Arena_struct(a, sj5_LL); \
  (*l##_end)->v = V; (*l##_end)->k = K; \ 
  l##_end = &((*l##_end)->next); }} while(0)

typedef struct str_node {
  struct str_node *next;
  str str;
} str_list;
typedef struct enum_node { 
  struct enum_node *next;
  str name;
  str_node keys;
  str_node values;
  bool standard;
};

Arena a;
void normalize(str agent_file, const char *output) {
  str agent_name = str_trim_end(agent_file, strl(".json5"));
  str src = read_file(&a, agent_file.str);

  // sj5_LL *insts = 0, **insts_end = &insts;
  // sj5_LL *enums = 0, **enums_end = &enums;
  // sj5_LL *structs = 0, **structs_end = &structs;
  // sj5_LL *cmds = 0, **cmds_end = &cmds;
  // sj5_LL *msgs = 0, **msgs_end = &msgs;

  LL_init(insts);
  LL_init(enums);
  LL_init(structs);
  LL_init(cmds);
  LL_init(msgs);

  enum_node *enums = 0, **enums_end = &enums;

  sj5_Reader r = sj5_reader(src.str, src.len);
  sj5_Value root = sj5_read(&r);
  sj5_Value key, val;
  while (sj5_iter_object(&r, root, &key, &val)) {
    if (str_eql(key.str, "_instances")) {
      // append instances to enums as agent_name
      LL_append(&a, enums, (sj5_Value) { .str = agent_name, .type = SJ5_STRING }, val);
    }
  }


}

int main(int argc, char **argv) {
  a = Arena_alloc((Arena){ .size = MB(128) });

  normalize(0, 0);

  return 123;
}
