#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#include <stdint.h>
#include <sodium.h>

int64_t ace_hash(unsigned char *input, int64_t len) {
  unsigned char hash[crypto_generichash_BYTES_MAX];
  int ret = crypto_generichash(hash, sizeof(hash), input, len, NULL, 0);
  return (*(uint64_t *)hash) & INT64_MAX;
}

int64_t ace_hash_int(int64_t x) {
  unsigned char hash[crypto_generichash_BYTES_MAX];
  int ret = crypto_generichash(hash, sizeof(hash), (unsigned char *)&x,
                               sizeof(x), NULL, 0);
  return (*(uint64_t *)hash) & INT64_MAX;
}
