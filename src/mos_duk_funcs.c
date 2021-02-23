#include "mos_duk_funcs.h"

#include "common/cs_dbg.h"

#include "mgos_timers.h"
#include "mgos_time.h"

#include "mos_duk_utils.h"

// taken from https://github.com/nkolban/duktape-esp32/blob/28b4fb194665039ec7a907d346e9c2cd44e387df/main/include/duktape_utils.h
#define ADD_FUNCTION(FUNCTION_NAME_STRING, FUNCTION_NAME, PARAM_COUNT) \
		duk_push_c_function(ctx, FUNCTION_NAME, PARAM_COUNT); \
		duk_put_prop_string(ctx, -2, FUNCTION_NAME_STRING)

#define ADD_GLOBAL_FUNCTION(FUNCTION_NAME_STRING, FUNCTION_NAME, PARAM_COUNT) \
		duk_push_c_function(ctx, FUNCTION_NAME, PARAM_COUNT); \
		duk_put_global_string(ctx, FUNCTION_NAME_STRING)

#define ADD_INT(INT_NAME, INT_VALUE) \
		duk_push_int(ctx, INT_VALUE); \
		duk_put_prop_string(ctx, -2, INT_NAME)

#define ADD_STRING(STRING_NAME, STRING_VALUE) \
		duk_push_string(ctx, STRING_VALUE); \
		duk_put_prop_string(ctx, -2, STRING_NAME)

#define ADD_BOOLEAN(BOOLEAN_NAME, BOOLEAN_VALUE) \
		duk_push_boolean(ctx, BOOLEAN_VALUE); \
		duk_put_prop_string(ctx, -2, BOOLEAN_NAME)

static void gen_random_id(char *s, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    int randomChar = rand()%(26+26+10);
    if (randomChar < 26)
      s[i] = 'a' + randomChar;
    else if (randomChar < 26+26)
      s[i] = 'A' + randomChar - 26;
    else
      s[i] = '0' + randomChar - 26 - 26;
  }
  s[len] = 0;
}

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

typedef struct MGOS_DUK_TIMER_CALLBACK_PAYLOAD {
  duk_context* ctx;
  char id[10]; // tcbXXXXXX
  mgos_timer_id native_timer_id;
} mgosTimerCallback;

static void mos_duk_timer_cb_handler(void* arg) {
  if (arg == NULL) {
    LOG(LL_ERROR, ("Invalid timer callback data."));
  }
  mgosTimerCallback timerCallbackData = *((mgosTimerCallback *) arg);
  duk_context* ctx = timerCallbackData.ctx;
  char* id = timerCallbackData.id;

  // obtain callback function
  duk_get_global_string(ctx, id);

  // check if it wasn't deleted
  if (!duk_is_function(ctx, -1)) {
    LOG(LL_ERROR, (
      "timer callback was deleted. Deleting this callback too. This is not ideal. "
      // TODO: add a way to remove a callback
      "Please remove callbacks by using TODO()"
    ));
    mgos_clear_timer(timerCallbackData.native_timer_id);
    free(arg);
    return;
  }

  /* Protected call, log callback errors. */
  duk_int_t rc = duk_pcall(ctx, 0);
  if (rc != 0) {
    mos_duk_log_error(ctx);
  }
  duk_pop(ctx);
}

static duk_ret_t mos_duk_func__set_interval(duk_context* ctx) {
  char id_buf[7] = {0};
  char callback_id[10] = "tcb";
  gen_random_id(id_buf, 6);
  strcat(callback_id, id_buf);
  callback_id[9] = '\0';
  
  long interval = (long) duk_require_uint(ctx, 1);
  duk_require_function(ctx, 0);
  duk_dup(ctx, 0); // store js callback
  // assign callback to a global var
  duk_put_global_string(ctx, callback_id);
  LOG(LL_DEBUG, ("Registering timer every %ld with callback as: %s", interval, callback_id));

  mgosTimerCallback* timerCallbackData = malloc(sizeof(mgosTimerCallback));
  if (timerCallbackData == NULL) {
    return DUK_RET_RANGE_ERROR;
  }

  timerCallbackData->ctx = ctx;
  strcpy(timerCallbackData->id, callback_id);
  mgos_timer_id id = mgos_set_timer(interval, MGOS_TIMER_REPEAT, mos_duk_timer_cb_handler, (void *)timerCallbackData);
  timerCallbackData->native_timer_id = id;

  return 0;
}

static duk_ret_t mos_duk_func__mos_timers_uptime(duk_context* ctx) {
  double uptime = mgos_uptime();
  duk_push_number(ctx, uptime);
  return 1;
}

void mos_duk_define_functions(duk_context* ctx) {
  // duk_int_t rc;

  // print(...)
  duk_push_c_function(ctx, &mos_duk_func_native_print, DUK_VARARGS);
  duk_put_global_string(ctx, "print");

  // console
  duk_push_global_object(ctx);
  mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "assert", LOG_ASSERT);
	mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "log", LOG_INFO);
  mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "info", LOG_INFO);
	mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "debug", LOG_DEBUG);
  mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "error", LOG_ERROR);
  mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "warn", LOG_WARN);
  duk_put_global_string(ctx, "console");

  // global utils
  ADD_GLOBAL_FUNCTION("setInterval", mos_duk_func__set_interval, 2);
  // TODO: setTimeout

  duk_push_object(ctx);
  // MOS ADC
  // MOS App
  // MOS BitBang
  // MOS Config
  // MOS Debug
  // MOS Event
  // MOS I2C
  // MOS JSON
  // MOS Logging
  // MOS Membuf
  // MOS Net Events
  // MOS Onewire
  // MOS PWM
  // MOS SPI
  // MOS String
  // MOS System
  // MOS Time
  duk_push_object(ctx); // MOS.Time
  ADD_FUNCTION("uptime", mos_duk_func__mos_timers_uptime, 1);
  duk_put_prop_string(ctx, -2, "Time");
  // MOS Timers
  duk_push_object(ctx); // MOS.Timers
  ADD_FUNCTION("uptime", mos_duk_func__mos_timers_uptime, 1);
  duk_put_prop_string(ctx, -2, "Timers");
  // MOS UART
  // MOS Utils

  // MOS global object
  duk_put_global_string(ctx, "MOS");
}