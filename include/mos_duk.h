/*
 * Duktape wrapper API.
 */

#ifndef MOS_DUK_H_
#define MOS_DUK_H_

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "duktape.h"

/* Return global duktape instance. */
duk_context* mgos_duk_get_global(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif