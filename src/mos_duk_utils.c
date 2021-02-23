#include "mos_duk_utils.h"

#include "common/cs_dbg.h"

void mos_duk_log_error(duk_context *ctx) {
	duk_idx_t errObjIdx = duk_get_top_index(ctx); // err object
	duk_get_prop_string(ctx, errObjIdx, "name");
	duk_get_prop_string(ctx, errObjIdx, "message");
	duk_get_prop_string(ctx, errObjIdx, "lineNumber");
	duk_get_prop_string(ctx, errObjIdx, "stack");

	const char *name = duk_get_string(ctx, -4);
	const char *message = duk_get_string(ctx, -3);
	int lineNumber = duk_get_int(ctx, -2);
	const char *stack = duk_get_string(ctx, -1);
	LOG(LL_ERROR, (
    "JS Error: \n%s: %s\nline: %d\nStack: %s",
			name != NULL ? name : "NULL",
			message != NULL ? message : "NULL",
			lineNumber,
			stack != NULL ? stack : "NULL"
  ));
	duk_pop_n(ctx, 4);
}