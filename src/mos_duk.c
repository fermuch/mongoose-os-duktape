#include "mos_duk.h"

#include "common/cs_dbg.h"
#include "common/platform.h"

#include "mgos_app.h"
#include "mgos_event.h"
#include "mgos_system.h"


static duk_context* ctx = NULL;

static void mos_duk_fatal_error_handler(void *udata, const char *msg) {
  (void) udata;
  LOG(LL_ERROR, ("*** FATAL ERROR: %s\n", (msg ? msg : "no message")));
  abort();
}

static void mos_duk_log_error(duk_context *ctx) {
	duk_idx_t errObjIdx = duk_get_top_index(ctx);
	duk_get_prop_string(ctx, errObjIdx, "name");
	duk_get_prop_string(ctx, errObjIdx, "message");
	duk_get_prop_string(ctx, errObjIdx, "lineNumber");
	duk_get_prop_string(ctx, errObjIdx, "stack");
	// [0] - Err object
	// [1] - name
	// [2] - message
	// [3] - lineNumber
	// [4] - Stack

	const char *name = duk_get_string(ctx, -4);
	const char *message = duk_get_string(ctx, -3);
	int lineNumber = duk_get_int(ctx, -2);
	const char *stack = duk_get_string(ctx, -1);
	LOG(LL_ERROR, ("JS Error: \n%s: %s\nline: %d\nStack: %s",
			name!=NULL?name:"NULL",
			message!=NULL?message:"NULL",
			lineNumber,
			stack!=NULL?stack:"NULL"));
	duk_pop_n(ctx, 4);
	// [0] - Err
}

static duk_ret_t native_print(duk_context *ctx) {
  // if (!duk_check_type_mask(ctx, -1, DUK_TYPE_STRING)) {
  //   return DUK_ERR_TYPE_ERROR;
  // }

	duk_push_string(ctx, " ");
	duk_insert(ctx, 0);
	duk_join(ctx, duk_get_top(ctx) - 1);
  LOG(LL_DEBUG, ("[JS]> %s", duk_safe_to_string(ctx, -1)));
  duk_pop(ctx);
	return 0;
}

bool mgos_duk_init(void) {
  /* Initialize Duktape engine */
  int mem1, mem2;
  mem1 = mgos_get_free_heap_size();

  ctx = duk_create_heap(NULL, NULL, NULL, NULL, mos_duk_fatal_error_handler);

  LOG(LL_DEBUG, ("Creating print function"));
  duk_push_c_function(ctx, &native_print, DUK_VARARGS);
  duk_put_global_string(ctx, "print");
  // duk_def_prop(ctx, -3, DUK_DEFPROP_HAVE_VALUE | DUK_DEFPROP_SET_WRITABLE | DUK_DEFPROP_SET_CONFIGURABLE);
  
  LOG(LL_DEBUG, ("Calling eval"));
  duk_push_string(ctx,
    "if (typeof Duktape !== 'object') {\n"
    "    print('not Duktape');\n"
    "} else if (Duktape.version >= 20403) {\n"
    "    print('Duktape 2.4.3 or higher');\n"
    "} else if (Duktape.version >= 10500) {\n"
    "    print('Duktape 1.5.0 or higher (but lower than 2.4.3)');\n"
    "} else {\n"
    "    print('Duktape lower than 1.5.0');\n"
    "}"
  );
  if (duk_peval(ctx) != 0) {
    // LOG(LL_ERROR, ("Script error: %s\n", duk_safe_to_string(ctx, -1)));
    mos_duk_log_error(ctx);
  }
  duk_pop(ctx);

  LOG(LL_DEBUG, ("Calling invalid function"));
  duk_push_string(ctx, "print(JSON.stringify(Duktape.info()))");
  if (duk_peval(ctx) != 0) {
    // LOG(LL_ERROR, ("Script error: %s\n", duk_safe_to_string(ctx, -1)));
    mos_duk_log_error(ctx);
  }
  duk_pop(ctx);

  mem2 = mgos_get_free_heap_size();

  LOG(LL_DEBUG, ("Killing duk..."));
  duk_destroy_heap(ctx);

  LOG(LL_DEBUG,
      ("Duktape memory stat: before init: %d after init: %d", mem1, mem2));
  return true;
}

duk_context* mgos_duk_get_global(void) {
  return ctx;
}