#ifndef MOS_DUK_UTILS_H_
#define MOS_DUK_UTILS_H_

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "duktape.h"

void mos_duk_log_error(duk_context *ctx);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif