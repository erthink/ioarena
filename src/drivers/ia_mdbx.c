﻿
/*
 * ioarena: embedded storage benchmarking
 *
 * Copyright (c) Dmitry Simonenko
 * Copyright (c) Leonid Yuriev
 * BSD License
 */

#include "ioarena.h"
#include "mdbx.h"

struct iaprivate {
  MDBX_env *env;
  MDBX_dbi dbi;
};

#define INVALID_DBI ((MDBX_dbi)-1)

struct mdbx_opts {
  int8_t liforeclaim;
  int8_t coalesce;
  int8_t exclusive;
  int8_t pageperturb;
  int8_t nomeminit;
  int8_t nordahead;
  int8_t nometasync;
};

static struct mdbx_opts globals;

static int ia_mdbx_option(iacontext *ctx, const char *arg) {
  if (ctx)
    return 0 /* no any non-global options */;

  if (strcmp(arg, "--help") == 0) {
    ia_log("  -o %s=<ON|OFF>", "LIFORECLAIM");
    ia_log("  -o %s=<ON|OFF>", "COALESCE");
    ia_log("  -o %s=<ON|OFF>", "EXCLUSIVE");
    ia_log("  -o %s=<ON|OFF>", "PAGEPERTURB");
    ia_log("  -o %s=<ON|OFF>", "NOMEMINIT");
    ia_log("  -o %s=<ON|OFF>", "NORDAHEAD");
    ia_log("  -o %s=<ON|OFF>", "NOMETASYNC");
    return 0;
  }

  int done = 1;
  while (*arg && done > 0) {
    done = ia_parse_option_bool(&arg, "LIFORECLAIM", &globals.liforeclaim);
    if (!done)
      done = ia_parse_option_bool(&arg, "COALESCE", &globals.coalesce);
    if (!done)
      done = ia_parse_option_bool(&arg, "EXCLUSIVE", &globals.exclusive);
    if (!done)
      done = ia_parse_option_bool(&arg, "PAGEPERTURB", &globals.pageperturb);
    if (!done)
      done = ia_parse_option_bool(&arg, "NOMEMINIT", &globals.nomeminit);
    if (!done)
      done = ia_parse_option_bool(&arg, "NORDAHEAD", &globals.nordahead);
    if (!done)
      done = ia_parse_option_bool(&arg, "NOMETASYNC", &globals.nometasync);
  }

  if (done == 1)
    return 0;
  ia_log("%s: invalid option or value `%s`", "mdbx", arg);
  return done ? done : -1;
}

static int peek_option_bool(int dflt, int opt, int8_t from) {
  switch (from) {
  default:
    ia_log("error: invalid bool-option value %d", from);
    ia_fatal(__func__);
    /* fall through */
  case ia_opt_bool_default:
    return dflt;
  case ia_opt_bool_off:
    return dflt & ~opt;
  case ia_opt_bool_on:
    return dflt | opt;
  }
}

struct iacontext {
  MDBX_txn *txn;
  MDBX_cursor *cursor;
};

static void ia_mdbx_dbglog(MDBX_log_level_t loglevel, const char *function,
                           int line, const char *fmt, va_list args) {
  (void)loglevel;
  (void)function;
  (void)line;
  ia_vlog(fmt, args);
}

static int ia_mdbx_open(const char *datadir) {
  unsigned modeflags;
  iadriver *drv = ioarena.driver;

  drv->priv = calloc(1, sizeof(iaprivate));
  if (drv->priv == NULL)
    return -1;

  mdbx_setup_debug(MDBX_LOG_NOTICE, MDBX_DBG_DONTCHANGE, ia_mdbx_dbglog);

  iaprivate *self = drv->priv;
  self->dbi = INVALID_DBI;
  int rc = mdbx_env_create(&self->env);
  if (rc != MDBX_SUCCESS)
    goto bailout;

  const size_t kilo = 1024;
  const size_t mega = kilo << 10;
  const size_t giga = mega << 10;
  /* FIXME: define values via command-line args */
  rc = mdbx_env_set_geometry(
      self->env,
#if 1
      /* 4Gb initial DB size,
       * to make the benchmark conditions the same as for LMDB */
      4 * giga /* size_lower */, 4 * giga /* size_now */,
#else
      /* dynamic DB size 1Mb..128Gb */
      1 * mega /* size_lower */, 64 * mega /* size_now */,
#endif
      128 * giga /* size_upper */, 64 * mega /* growth_step */,
      128 * mega /* shrink_threshold */, -1 /* default pagesize */);
  if (rc != MDBX_SUCCESS)
    goto bailout;

  switch (ioarena.conf.syncmode) {
  case IA_SYNC:
    modeflags = MDBX_SYNC_DURABLE | MDBX_LIFORECLAIM;
    break;
  case IA_LAZY:
    modeflags = MDBX_SAFE_NOSYNC | MDBX_NOMETASYNC;
    break;
  case IA_NOSYNC:
    modeflags = MDBX_WRITEMAP | MDBX_UTTERLY_NOSYNC;
    break;
  default:
    ia_log("error: %s(): unsupported syncmode %s", __func__,
           ia_syncmode2str(ioarena.conf.syncmode));
    return -1;
  }

  switch (ioarena.conf.walmode) {
  case IA_WAL_INDEF:
    break;
  case IA_WAL_OFF:
    modeflags &= ~MDBX_NOMETASYNC;
    break;
  case IA_WAL_ON:
    modeflags |= MDBX_NOMETASYNC;
    break;
  default:
    ia_log("error: %s(): unsupported walmode %s", __func__,
           ia_walmode2str(ioarena.conf.walmode));
    return -1;
  }

  modeflags =
      peek_option_bool(modeflags, MDBX_LIFORECLAIM, globals.liforeclaim);
  modeflags = peek_option_bool(modeflags, MDBX_COALESCE, globals.coalesce);
  modeflags = peek_option_bool(modeflags, MDBX_EXCLUSIVE, globals.exclusive);
  modeflags =
      peek_option_bool(modeflags, MDBX_PAGEPERTURB, globals.pageperturb);
  modeflags = peek_option_bool(modeflags, MDBX_NOMEMINIT, globals.nomeminit);
  modeflags = peek_option_bool(modeflags, MDBX_NORDAHEAD, globals.nordahead);
  modeflags = peek_option_bool(modeflags, MDBX_NOMETASYNC, globals.nometasync);

  rc = mdbx_env_open(self->env, datadir, modeflags, 0644);
  if (rc != MDBX_SUCCESS)
    goto bailout;
  return 0;

bailout:
  ia_log("error: %s, %s (%d)", __func__, mdbx_strerror(rc), rc);
  return -1;
}

static int ia_mdbx_close(void) {
  iaprivate *self = ioarena.driver->priv;
  if (self) {
    ioarena.driver->priv = NULL;
    if (self->dbi != INVALID_DBI)
      mdbx_dbi_close(self->env, self->dbi);
    if (self->env)
      mdbx_env_close(self->env);
    free(self);
  }
  return 0;
}

static iacontext *ia_mdbx_thread_new(void) {
  iaprivate *self = ioarena.driver->priv;
  int rc, err;

  if (self->dbi == INVALID_DBI) {
    /* just bind thread to DB */
    MDBX_txn *txn = NULL;
    rc = mdbx_txn_begin(self->env, NULL, MDBX_TXN_RDONLY, &txn);
    if (rc != MDBX_SUCCESS)
      goto bailout;

    rc = mdbx_dbi_open(txn, NULL, 0, &self->dbi);
    err = mdbx_txn_abort(txn);
    if (err != MDBX_SUCCESS)
      ia_log("error: %s, %s (%d)", "mdbx_txn_abort", mdbx_strerror(err), err);
    assert(err == 0);
    if (rc != MDBX_SUCCESS)
      goto bailout;

    assert(self->dbi != INVALID_DBI);
  }

  iacontext *ctx = calloc(1, sizeof(iacontext));
  return ctx;

bailout:
  ia_log("error: %s, %s (%d)", __func__, mdbx_strerror(rc), rc);
  return NULL;
}

static void ia_mdbx_thread_dispose(iacontext *ctx) {
  if (ctx->cursor)
    mdbx_cursor_close(ctx->cursor);
  if (ctx->txn) {
    int err = mdbx_txn_abort(ctx->txn);
    if (err != MDBX_SUCCESS)
      ia_log("error: %s, %s (%d)", "mdbx_txn_abort", mdbx_strerror(err), err);
    assert(err == 0);
    ctx->txn = NULL;
  }
  free(ctx);
}

static int ia_mdbx_begin(iacontext *ctx, iabenchmark step) {
  iaprivate *self = ioarena.driver->priv;
  int rc = 0;
  assert(self->dbi != INVALID_DBI);

  switch (step) {
  case IA_SET:
  case IA_BATCH:
  case IA_CRUD:
  case IA_DELETE:
    if (ctx->cursor) {
      /* LY: cursor could NOT be reused for read/write. */
      mdbx_cursor_close(ctx->cursor);
      ctx->cursor = NULL;
    }
    if (ctx->txn) {
      /* LY: transaction could NOT be reused for read/write. */
      rc = mdbx_txn_abort(ctx->txn);
      ctx->txn = NULL;
      if (rc != MDBX_SUCCESS) {
        ia_log("error: %s, %s (%d)", "mdbx_txn_abort", mdbx_strerror(rc), rc);
        goto bailout;
      }
    }
    rc = mdbx_txn_begin(self->env, NULL, 0, &ctx->txn);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    break;

  case IA_ITERATE:
  case IA_GET:
    if (ctx->txn) {
      rc = mdbx_txn_renew(ctx->txn);
      if (rc != MDBX_SUCCESS) {
        ia_log("warning: %s, %s (%d)", "mdbx_txn_renew", mdbx_strerror(rc), rc);
        rc = mdbx_txn_abort(ctx->txn);
        ctx->txn = NULL;
        if (rc != MDBX_SUCCESS) {
          ia_log("error: %s, %s (%d)", "mdbx_txn_abort", mdbx_strerror(rc), rc);
          goto bailout;
        }
      }
    }
    if (ctx->txn == NULL) {
      rc = mdbx_txn_begin(self->env, NULL, MDBX_TXN_RDONLY, &ctx->txn);
      if (rc != MDBX_SUCCESS)
        goto bailout;
    }

    if (step == IA_ITERATE) {
      if (ctx->cursor) {
        rc = mdbx_cursor_renew(ctx->txn, ctx->cursor);
        if (rc != MDBX_SUCCESS) {
          ia_log("warning: %s, %s (%d)", "mdbx_cursor_renew", mdbx_strerror(rc),
                 rc);
          mdbx_cursor_close(ctx->cursor);
          ctx->cursor = NULL;
        }
      }
      if (ctx->cursor == NULL) {
        rc = mdbx_cursor_open(ctx->txn, self->dbi, &ctx->cursor);
        if (rc != MDBX_SUCCESS)
          goto bailout;
      }
    }
    break;

  default:
    assert(0);
    rc = -1;
  }

  return rc;

bailout:
  ia_log("error: %s, %s, %s (%d)", __func__, ia_benchmarkof(step),
         mdbx_strerror(rc), rc);
  return -1;
}

static int ia_mdbx_done(iacontext *ctx, iabenchmark step) {
  int rc;

  switch (step) {
  case IA_SET:
  case IA_BATCH:
  case IA_CRUD:
  case IA_DELETE:
    rc = mdbx_txn_commit(ctx->txn);
    if (rc != MDBX_SUCCESS) {
      int err = mdbx_txn_abort(ctx->txn);
      if (err != MDBX_SUCCESS)
        ia_log("error: %s, %s (%d)", "mdbx_txn_abort", mdbx_strerror(err), err);
      ctx->txn = NULL;
      goto bailout;
    }
    ctx->txn = NULL;
    break;

  case IA_ITERATE:
  case IA_GET:
    rc = mdbx_txn_reset(ctx->txn);
    if (rc != MDBX_SUCCESS) {
      ia_log("error: %s, %s (%d)", "mdbx_txn_reset", mdbx_strerror(rc), rc);
      goto bailout;
    }
    break;

  default:
    assert(0);
    rc = -1;
  }

  return rc;

bailout:
  ia_log("error: %s, %s, %s (%d)", __func__, ia_benchmarkof(step),
         mdbx_strerror(rc), rc);
  return -1;
}

static int ia_mdbx_next(iacontext *ctx, iabenchmark step, iakv *kv) {
  iaprivate *self = ioarena.driver->priv;
  MDBX_val k, v;
  int rc;

  switch (step) {
  case IA_SET:
    k.iov_base = kv->k;
    k.iov_len = kv->ksize;
    v.iov_base = kv->v;
    v.iov_len = kv->vsize;
    rc = mdbx_put(ctx->txn, self->dbi, &k, &v, 0);
    if (rc != MDBX_SUCCESS)
      goto bailout;
    break;

  case IA_DELETE:
    k.iov_base = kv->k;
    k.iov_len = kv->ksize;
    rc = mdbx_del(ctx->txn, self->dbi, &k, 0);
    if (rc == MDBX_NOTFOUND)
      rc = ENOENT;
    else if (rc != MDBX_SUCCESS)
      goto bailout;
    break;

  case IA_ITERATE:
    rc = mdbx_cursor_get(ctx->cursor, &k, &v, MDBX_NEXT);
    if (rc == MDBX_SUCCESS) {
      kv->k = k.iov_base;
      kv->ksize = k.iov_len;
      kv->v = v.iov_base;
      kv->vsize = v.iov_len;
    } else if (rc == MDBX_NOTFOUND) {
      kv->k = NULL;
      kv->ksize = 0;
      kv->v = NULL;
      kv->vsize = 0;
      rc = ENOENT;
    } else {
      goto bailout;
    }
    break;

  case IA_GET:
    k.iov_base = kv->k;
    k.iov_len = kv->ksize;
    rc = mdbx_get(ctx->txn, self->dbi, &k, &v);
    if (rc != MDBX_SUCCESS) {
      if (rc != MDBX_NOTFOUND)
        goto bailout;
      rc = ENOENT;
    }
    break;

  default:
    assert(0);
    rc = -1;
  }

  return rc;

bailout:
  ia_log("error: %s, %s, %s (%d)", __func__, ia_benchmarkof(step),
         mdbx_strerror(rc), rc);
  return -1;
}

iadriver ia_mdbx = {.name = "mdbx",
                    .priv = NULL,
                    .open = ia_mdbx_open,
                    .close = ia_mdbx_close,
                    .option = ia_mdbx_option,

                    .thread_new = ia_mdbx_thread_new,
                    .thread_dispose = ia_mdbx_thread_dispose,
                    .begin = ia_mdbx_begin,
                    .next = ia_mdbx_next,
                    .done = ia_mdbx_done};
