#include "mos_duk.h"
#include "duk_module_node.h"

#include "common/cs_dbg.h"
#include "common/platform.h"

#include "mgos_app.h"
#include "mgos_event.h"
#include "mgos_system.h"


static duk_context* ctx = NULL;

duk_context* mgos_duk_get_global(void) {
  return ctx;
}

static void mos_duk_fatal_error_handler(void *udata, const char *msg) {
  (void) udata;
  LOG(LL_ERROR, ("*** FATAL ERROR: %s\n", (msg ? msg : "no message")));
  // TODO: make restart on fatal configurable
  mgos_system_restart();
}

static bool mos_duk_file_exists(const char *fname) {
  LOG(LL_DEBUG, (
    "mos_duk_file_exists: fname:'%s'",
      fname
  ));

  struct stat st;
  if (stat(fname, &st) != 0) return false;
  return true;
}

// taken from: https://github.com/cesanta/mjs/blob/c1597477f4062756878ddf0e8194ccaf05cd454c/common/cs_file.c
static char* mos_duk_read_file(const char *path, size_t *size) {
  FILE *fp;
  char *data = NULL;
  if ((fp = fopen(path, "rb")) == NULL) {
  } else if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
  } else {
    *size = ftell(fp);
    // XXX: maybe we could use duk_alloc to use its GC
    data = (char *) malloc(*size + 1);
    if (data != NULL) {
      fseek(fp, 0, SEEK_SET); /* Some platforms might not have rewind(), Oo */
      if (fread(data, 1, *size, fp) != *size) {
        free(data);
        return NULL;
      }
      data[*size] = '\0';
    }
    fclose(fp);
  }
  return data;
}

static duk_ret_t mos_duk_resolve_module_handler(duk_context *ctx) {
  const char *module_id;
	const char *parent_id;

	module_id = duk_require_string(ctx, 0);
	parent_id = duk_require_string(ctx, 1);

  bool is_simple = mos_duk_file_exists(module_id);
  if (is_simple) {
    duk_push_string(ctx, module_id);
  } else {
    // Append .js to the end of the file
    char buf[strlen(module_id) + 4];
    snprintf(buf, sizeof(buf), "%s.js", module_id);
    bool is_js = mos_duk_file_exists(buf);

    // We ran out of options.
    // XXX: Maybe we should try .mjs too?
    if (!is_js) {
      (void) duk_type_error(ctx, "cannot find module: %s", module_id);
      return DUK_RET_ERROR;
    }

    // XXX: should we compute the hash of the module?
    duk_push_string(ctx, buf);
  }

	LOG(LL_DEBUG, (
    "mos_duk_resolve_module_handler: id:'%s', parent-id:'%s', resolve-to:'%s'",
      module_id,
      parent_id,
      duk_get_string(ctx, -1)
  ));

	return 1;
}

static duk_ret_t mos_duk_load_module_handler(duk_context *ctx) {
  const char *filename;
  const char *module_id;

  module_id = duk_require_string(ctx, 0);
  duk_get_prop_string(ctx, 2, "filename");
  filename = duk_require_string(ctx, -1);

  LOG(LL_DEBUG, ("mos_duk_load_module_handler: id:'%s', filename:'%s'", module_id, filename));

  size_t size;
  char *source_code = mos_duk_read_file(filename, &size);
  if (source_code == NULL) {
    LOG(LL_ERROR, ("cannot find module: %s", module_id));
    (void) duk_type_error(ctx, "cannot find module: %s", module_id);
    return DUK_RET_ERROR;
  }

  LOG(LL_DEBUG, ("File: %s", source_code));
  const char* dukstr = duk_push_string(ctx, source_code);
  free(source_code);
  LOG(LL_DEBUG, ("Duktape STR: %s", dukstr));
  return 1;
}

static void mos_duk_log_error(duk_context *ctx) {
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

static duk_ret_t native_print(duk_context *ctx) {
	duk_push_string(ctx, " ");
	duk_insert(ctx, 0);
	duk_join(ctx, duk_get_top(ctx) - 1);
  LOG(LL_DEBUG, ("[JS]> %s", duk_safe_to_string(ctx, -1)));
  duk_pop(ctx);
	return 0;
}

static void mos_duk_init_done_handler(int ev, void *ev_data, void *userdata) {
  LOG(LL_DEBUG, ("Calling main function"));
  duk_push_string(ctx,
    "if (typeof Duktape !== 'object') {\n"
    "    print('not Duktape');\n"
    "} else if (Duktape.version >= 20403) {\n"
    "    print('Duktape 2.4.3 or higher');\n"
    "} else if (Duktape.version >= 10500) {\n"
    "    print('Duktape 1.5.0 or higher (but lower than 2.4.3)');\n"
    "} else {\n"
    "    print('Duktape lower than 1.5.0');\n"
    "}\n\n\n"
    "try { require('foo'); } catch (e) { print('problem catched!'); print('Error was: ', e); }"
  );
  duk_ret_t rc = duk_module_node_peval_main(ctx, "index.js");
  if (rc != 0) {
    LOG(LL_DEBUG, ("about to call mos_duk_log_error"));
    mos_duk_log_error(ctx);
  }
}

bool mgos_duk_init(void) {
  /* Initialize Duktape engine */
  int mem1, mem2;
  mem1 = mgos_get_free_heap_size();

  ctx = duk_create_heap(NULL, NULL, NULL, NULL, mos_duk_fatal_error_handler);

  LOG(LL_DEBUG, ("Creating NodeJS-style resolvers"));
  duk_push_object(ctx);
  duk_push_c_function(ctx, mos_duk_resolve_module_handler, DUK_VARARGS);
  duk_put_prop_string(ctx, -2, "resolve");
  duk_push_c_function(ctx, mos_duk_load_module_handler, DUK_VARARGS);
  duk_put_prop_string(ctx, -2, "load");
  duk_module_node_init(ctx);

  LOG(LL_DEBUG, ("Creating print function"));
  duk_push_c_function(ctx, &native_print, DUK_VARARGS);
  duk_put_global_string(ctx, "print");

  mgos_event_add_handler(MGOS_EVENT_INIT_DONE, mos_duk_init_done_handler, NULL);

  mem2 = mgos_get_free_heap_size();

  // LOG(LL_DEBUG, ("Killing duk..."));
  // duk_destroy_heap(ctx);

  LOG(LL_DEBUG,
      ("Duktape memory stat: before init: %d after init: %d (%d kb)", mem1, mem2, (mem1 - mem2) / 1024));
  return true;
}
