/*
 *  gensio - A library for abstracting stream I/O
 *  Copyright (C) 2018  Corey Minyard <minyard@acm.org>
 *
 *  SPDX-License-Identifier: LGPL-2.1-only
 */

#ifndef GENSIO_FILTER_SCRIPT_H
#define GENSIO_FILTER_SCRIPT_H

#include <gensio/gensio_base.h>

int gensio_script_filter_alloc(struct gensio_os_funcs *o,
			       const char * const args[],
			       struct gensio_filter **rfilter);

#endif /* GENSIO_FILTER_SCRIPT_H */
