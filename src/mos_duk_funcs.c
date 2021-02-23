#include "mos_duk_funcs.h"

#include "common/cs_dbg.h"

// taken from https://github.com/svaarala/duktape/blob/master/extras/console/duk_console.c
static void mos_duk_reg_vararg_func(duk_context *ctx, duk_c_function func, const char *name, duk_uint_t flags) {
	duk_push_c_function(ctx, func, DUK_VARARGS);
	duk_push_string(ctx, "name");
	duk_push_string(ctx, name);
	duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_FORCE);  /* Improve stacktraces by displaying function name */
	duk_set_magic(ctx, -1, (duk_int_t) flags);
	duk_put_prop_string(ctx, -2, name);
}

static duk_ret_t mos_duk_func_native_print(duk_context* ctx) {
	duk_push_string(ctx, " ");
	duk_insert(ctx, 0);
	duk_join(ctx, duk_get_top(ctx) - 1);
  LOG(LL_DEBUG, ("[JS]> %s", duk_safe_to_string(ctx, -1)));
  duk_pop(ctx);
	return 0;
}

enum MOS_DUK_FUNC_LOG_TYPES {
  LOG_ASSERT,
  LOG_INFO,
  LOG_DEBUG,
  LOG_ERROR,
  LOG_WARN
};
static duk_ret_t mos_duk_func_log(duk_context* ctx) {
  duk_uint_t flag = (duk_uint_t) duk_get_current_magic(ctx);

  duk_push_string(ctx, " ");
	duk_insert(ctx, 0);
	duk_join(ctx, duk_get_top(ctx) - 1);
  switch (flag) {
    case LOG_ASSERT:
      LOG(LL_WARN, ("[JS:ASSERT]> %s", duk_safe_to_string(ctx, -1)));
      break;
    case LOG_INFO:
      LOG(LL_INFO, ("[JS:I]> %s", duk_safe_to_string(ctx, -1)));
      break;
    case LOG_DEBUG:
      LOG(LL_DEBUG, ("[JS:D]> %s", duk_safe_to_string(ctx, -1)));
      break;
    case LOG_ERROR:
      LOG(LL_ERROR, ("[JS:E]> %s", duk_safe_to_string(ctx, -1)));
      break;
    case LOG_WARN:
      LOG(LL_WARN, ("[JS:W]> %s", duk_safe_to_string(ctx, -1)));
      break;
  }
  duk_pop(ctx);
  return 0;
}

void mos_duk_define_functions(duk_context* ctx) {
  // print(...)
  duk_push_c_function(ctx, &mos_duk_func_native_print, DUK_VARARGS);
  duk_put_global_string(ctx, "print");

  // console
  duk_push_object(ctx);
  mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "assert", LOG_ASSERT);
	mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "log", LOG_INFO);
	mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "debug", LOG_DEBUG);
  mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "error", LOG_ERROR);
  mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "warn", LOG_WARN);
  duk_put_global_string(ctx, "console");
}