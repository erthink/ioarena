#pragma once
#ifndef IA_LOG_H_
#define IA_LOG_H_

/*
 * ioarena: embedded storage benchmarking
 *
 * Copyright (c) ioarena authors
 * BSD License
 */

void ia_vlog(const char *, va_list);

static inline __attribute__((format(printf, 1, 2))) void ia_log(const char *fmt,
                                                                ...) {
  va_list args;
  va_start(args, fmt);
  ia_vlog(fmt, args);
  va_end(args);
}

#endif
