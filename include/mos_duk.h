/*
 * mJS wrapper API.
 */

#ifndef MOS_MJS_H_
#define MOS_MJS_H_

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "duktape.h"

/* Return global mJS instance. */
duk_context* mgos_duk_get_global(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif