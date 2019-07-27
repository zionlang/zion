#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gc/gc.h>

void zion_init() {
	/* initialize the collector */
	GC_INIT();

	/* start mutator ... */
}

void *zion_malloc(uint64_t cb) {
  void *pb = GC_MALLOC(cb);
  // printf("allocated %" PRId64 " bytes at 0x%08" PRIx64 "\n", cb, (uint64_t)pb);
  return pb;
}

int zion_strlen(const char *sz) {
  return strlen(sz);
}

void *zion_print_int64(int64_t x) {
  printf("%" PRId64 "\n", x);
  return 0;
}

int zion_write_char(int64_t fd, char x) {
  char sz[] = {x};
  return write(fd, sz, 1);
}

char *zion_itoa(int64_t x) {
  char sz[128];
  if (snprintf(sz, sizeof(sz), "%" PRId64, x) < 1) {
    perror("Failed in zion_itoa");
    exit(1);
  }
  return GC_strndup(sz, strlen(sz));
}

char *zion_ftoa(double x) {
  char sz[128];
  /* IEEE double precision floats have about 15 decimal digits of precision */
  if (snprintf(sz, sizeof(sz), "%.15f", x) < 1) {
    perror("Failed in zion_ftoa");
    exit(1);
  }
  return GC_strndup(sz, strlen(sz));
}

void zion_pass_test() {
  write(1, "PASS\n", 5);
}

int zion_puts(char *sz) {
  if (sz == 0) {
    const char *error = "attempt to puts a null pointer!\n";
    write(1, error, zion_strlen(error));
  }
  write(1, sz, zion_strlen(sz));
  write(1, "\n", 1);
  return 0;
}
