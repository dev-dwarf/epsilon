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



int main(int argc, char **argv) {
  Arena a = Arena_alloc((Arena){ .size = MB(32) });

  str src = read_file(&a, "example.json5");

  sj5_Reader r = sj5_reader(src.str, src.len);
  sj5_Value root = sj5_read(&r);
  sj5_Value key, val;
  while (sj5_iter_object(&r, root, &key, &val)) {
    printf("\n");
  }

  return 123;
}
