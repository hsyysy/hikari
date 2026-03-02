#include <hikari/memory.h>

#include <stdlib.h>

#include <hikari/log.h>

void *
hikari_malloc(size_t size)
{
  void *ptr = malloc(size);
  if (ptr == NULL) {
    hikari_log_error("out of memory (malloc %zu bytes)", size);
    abort();
  }
  return ptr;
}

void *
hikari_calloc(size_t number, size_t size)
{
  void *ptr = calloc(number, size);
  if (ptr == NULL) {
    hikari_log_error("out of memory (calloc %zu * %zu bytes)", number, size);
    abort();
  }
  return ptr;
}

void
hikari_free(void *ptr)
{
  free(ptr);
}
