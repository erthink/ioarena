
/*
 * ioarena: embedded storage benchmarking
 *
 * Copyright (c) ioarena authors
 * BSD License
 */

#include "ioarena.h"

void ia_vlog(const char *fmt, va_list args) {
  vprintf(fmt, args);
  printf("\n");
  fflush(NULL);
}
