#include <stdio.h>
#include <stdlib.h>

void foo(FILE *f) { fclose(f); }

int main(int argc, char **argv) {
  FILE *f;

  int i;

  while ((i = getc(f)) != EOF)
    putc(i, f);

  f = fopen(argv[1], "a");

  foo(f);

  return 0;
}