/* SPDX-License-Identifier: MIT */
/*
 * An HTTP/1.1 client. See http.c for why the transport is split in two and
 * what that costs.
 */
#ifndef FL_HTTP_H
#define FL_HTTP_H

#include "vm.h"

/* register __http_request. called from register_sys_natives(). */
void register_http_natives(VM *vm);

#endif /* FL_HTTP_H */