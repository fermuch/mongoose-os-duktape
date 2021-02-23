/*
 * Duktape functions for interaction with Mongoose.
 */

#ifndef MOS_DUK_FUNCS_H_
#define MOS_DUK_FUNCS_H_

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "duktape.h"

/* Define duktape functions. */
void mos_duk_define_functions(duk_context* ctx);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif