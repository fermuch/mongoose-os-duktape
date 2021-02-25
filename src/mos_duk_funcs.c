#include "mos_duk_funcs.h"

#include "common/cs_dbg.h"

#include "mgos_timers.h"
#include "mgos_time.h"
#include "mgos_adc.h"
#include "mgos_bitbang.h"
#include "mgos_config.h"
#include "mgos_sys_config.h"
#include "mgos_event.h"
#include "mgos_debug.h"
#include "mgos_gpio.h"
#include "mgos_system.h"
#include "mgos_time.h"

#include "mos_duk_utils.h"

// HACK: for some reason this function is unexported.
float mgos_conf_value_float(const void *cfg, const struct mgos_conf_entry *e);

// taken from https://github.com/nkolban/duktape-esp32/blob/28b4fb194665039ec7a907d346e9c2cd44e387df/main/include/duktape_utils.h
#define ADD_FUNCTION(FUNCTION_NAME_STRING, FUNCTION_NAME, PARAM_COUNT) \
		duk_push_c_function(ctx, FUNCTION_NAME, PARAM_COUNT); \
		duk_put_prop_string(ctx, -2, FUNCTION_NAME_STRING)

#define ADD_FUNCTION_W_FLAG(FUNCTION_NAME_STRING, FUNCTION_NAME, PARAM_COUNT, FLAG) \
    duk_push_c_function(ctx, FUNCTION_NAME, PARAM_COUNT); \
    duk_set_magic(ctx, -1, (duk_int_t) FLAG); \
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

typedef struct {
  duk_context* ctx;
  char id[10]; // tcbXXXXXX
  mgos_timer_id native_timer_id;
  bool delete_after; // delete after first execution
} mgosTimerCallback;

static void mos_duk_timer_cb_handler(void* arg) {
  if (arg == NULL) {
    LOG(LL_ERROR, ("Invalid timer callback data."));
    return;
  }
  mgosTimerCallback timerCallbackData = *((mgosTimerCallback *) arg);
  duk_context* ctx = timerCallbackData.ctx;
  char* id = timerCallbackData.id;
  bool delete_after = timerCallbackData.delete_after;

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

  duk_int_t rc = duk_pcall(ctx, 0);
  if (rc != 0) {
    mos_duk_log_error(ctx);
  }
  duk_pop(ctx);

  if (delete_after) {
    mgos_clear_timer(timerCallbackData.native_timer_id);
    free(arg);
  }
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
  timerCallbackData->delete_after = false;
  strcpy(timerCallbackData->id, callback_id);
  mgos_timer_id id = mgos_set_timer(interval, MGOS_TIMER_REPEAT, mos_duk_timer_cb_handler, (void *)timerCallbackData);
  timerCallbackData->native_timer_id = id;

  return 0;
}

static duk_ret_t mos_duk_func__set_timeout(duk_context* ctx) {
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
  LOG(LL_DEBUG, ("Registering timer after %ld with callback: %s()", interval, callback_id));

  mgosTimerCallback* timerCallbackData = malloc(sizeof(mgosTimerCallback));
  if (timerCallbackData == NULL) {
    return DUK_RET_RANGE_ERROR;
  }

  timerCallbackData->ctx = ctx;
  strcpy(timerCallbackData->id, callback_id);
  timerCallbackData->delete_after = true;
  mgos_timer_id id = mgos_set_timer(interval, 0, mos_duk_timer_cb_handler, (void *)timerCallbackData);
  timerCallbackData->native_timer_id = id;

  return 0;
}

static duk_ret_t mos_duk_func__mos_timers_uptime(duk_context* ctx) {
  double uptime = mgos_uptime();
  duk_push_number(ctx, uptime);
  return 1;
}

// MGOS.ADC.enable()
static duk_ret_t mos_duk_func__adc_enable(duk_context* ctx) {
  int pin;
  pin = duk_require_uint(ctx, 0);
  bool enabled = mgos_adc_enable(pin);
  duk_push_boolean(ctx, enabled);
  return 1;
}

// MGOS.ADC.read()
static duk_ret_t mos_duk_func__adc_read(duk_context* ctx) {
  int pin;
  pin = duk_require_int(ctx, 0);
  int value = mgos_adc_read_voltage(pin); // returns mV
  duk_push_uint(ctx, value);
  return 1;
}

// MGOS.BitBang.write()
#if MGOS_ENABLE_BITBANG
static duk_ret_t mos_duk_func__bitbang_write(duk_context* ctx) {
  duk_uint_t gpio = duk_require_uint(ctx, 0);
  duk_uint_t delay_unit = duk_require_uint(ctx, 1);
  duk_uint_t t0h = duk_require_uint(ctx, 2);
  duk_uint_t t0l = duk_require_uint(ctx, 3);
  duk_uint_t t1h = duk_require_uint(ctx, 4);
  duk_uint_t t1l = duk_require_uint(ctx, 5);
  duk_size_t sz;
  const uint8_t* data = duk_require_buffer_data(ctx, 6, &sz);

  LOG(LL_DEBUG, ("mgos_bitbang_write_bits: buf=%p, size=%lu\n", data, (unsigned long) sz));
  mgos_bitbang_write_bits(gpio, delay_unit, t0h, t0l, t1h, t1l, data, sz);
  return 0;
}
#endif

static duk_ret_t mos_duk_func__config_get(duk_context* ctx) {
  const char* path = duk_require_string(ctx, 0);

  const void *entry_ptr = mgos_conf_find_schema_entry(path, mgos_config_schema());
  if (entry_ptr == NULL) {
    return 0; // return undefined
  }

  enum mgos_conf_type conf_type = mgos_conf_value_type((void *)entry_ptr);
  LOG(LL_VERBOSE_DEBUG, ("mos_duk_func__config_get: Found '%s' with type: %d", path, conf_type));
  switch (conf_type) {
    case CONF_TYPE_INT:
      duk_push_int(ctx, mgos_conf_value_int(&mgos_sys_config, entry_ptr));
      break;
    case CONF_TYPE_BOOL:
      duk_push_boolean(ctx, mgos_conf_value_int(&mgos_sys_config, entry_ptr) != 0);
      break;
    case CONF_TYPE_DOUBLE:
      duk_push_number(ctx, mgos_conf_value_double(&mgos_sys_config, entry_ptr));
      break;
    case CONF_TYPE_STRING:
      duk_push_string(ctx, mgos_conf_value_string_nonnull(&mgos_sys_config, entry_ptr));
      break;
    case CONF_TYPE_OBJECT:
      // objects are unsupported for now
      LOG(LL_ERROR, ("Path '%s' is an object. Please be more specific.", path));
      duk_push_error_object(ctx, DUK_ERR_REFERENCE_ERROR, "Path '%s' is an object. Please be more specific.", path);
      return 1;
    case CONF_TYPE_UNSIGNED_INT:
      duk_push_uint(ctx, mgos_conf_value_int(&mgos_sys_config, entry_ptr));
      break;
    case CONF_TYPE_FLOAT:
      duk_push_number(ctx, mgos_conf_value_float(&mgos_sys_config, entry_ptr));
      break;
  }
  
  return 1;
}

static duk_ret_t mos_duk_func__config_set(duk_context* ctx) {
  // check if it is an object
  duk_require_object(ctx, 0);
  // now convert it to a JSON string
  duk_json_encode(ctx, 0);
  // and get it as a string
  const char *data = duk_require_string(ctx, 0);
  duk_bool_t commit = duk_require_boolean(ctx, 1);
  bool res = mgos_config_apply(data, commit);
  duk_push_boolean(ctx, res);
  return 1;
}

static duk_ret_t mos_duk_func__config_reset(duk_context* ctx) {
  int level = duk_require_int(ctx, 0);
  mgos_config_reset(level);
  return 0;
}

static duk_ret_t mos_duk_func__event_base_number(duk_context* ctx) {
  const char* name = duk_require_string(ctx, 0);
  size_t sz = strlen(name);
  if (sz != 3) {
    duk_push_error_object(ctx, DUK_ERR_ERROR, "baseNumber(id) only accepts 3 characters. Total given: %d", sz);
    return 1;
  }
  char a = name[0];
  char b = name[1];
  char c = name[2];
  uint16_t ev_number = ((a) << 24 | (b) << 16 | (c) << 8);
  duk_push_int(ctx, ev_number);
  return 1;
}

static duk_ret_t mos_duk_func__event_register(duk_context* ctx) {
  int event_number = duk_require_int(ctx, 0);
  const char* name = duk_require_string(ctx, 1);
  
  bool res = mgos_event_register_base(event_number, name);
  duk_push_boolean(ctx, res);
  return 1;
}

static duk_ret_t mos_duk_func__event_trigger(duk_context* ctx) {
  duk_int_t top = duk_get_top(ctx);
  if (top > 2 || top < 1) {
    duk_push_error_object(ctx, DUK_ERR_TYPE_ERROR, "At least one parameter is needed. Second parameter is callback data (Uint8Array).");
    return 1;
  }
  int event_number = duk_require_int(ctx, 0);
  duk_size_t sz;
  void *data = NULL;
  if (top == 2) {
    data = duk_require_buffer_data(ctx, 1, &sz);
  }
  int ret = mgos_event_trigger(event_number, data);
  duk_push_int(ctx, ret);
  return 1;
}

typedef struct {
  duk_context* ctx;
  char id[10]; // ecbXXXXXX
} mgosEventCallback;

static void mos_duk_event_cb_handler(int ev, void *ev_data, void *userdata) {
  if (userdata == NULL) {
    LOG(LL_ERROR, ("Invalid event callback data."));
    return;
  }
  mgosEventCallback eventCallbackData = *((mgosEventCallback *) userdata);
  duk_context* ctx = eventCallbackData.ctx;
  char* id = eventCallbackData.id;

  // obtain callback function
  duk_get_global_string(ctx, id);

  // check if it wasn't deleted
  if (!duk_is_function(ctx, -1)) {
    LOG(LL_ERROR, ("event callback was deleted. This shouldn't happen"));
    if (userdata != NULL) {
      // at this point let's free the user data. Why not, if we can't use it.
      free(userdata);
    }
    return;
  }

  // these are the func parameters
  //    first parameter is event id
  duk_push_int(ctx, ev);
  //    second parameter is buffer (evdata) or null
  if (ev_data == NULL) {
    duk_push_null(ctx);
  } else {
    struct mgos_debug_hook_arg *ptr = (struct mgos_debug_hook_arg *) ev_data;
    duk_uint8_t *duk_ptr = duk_push_fixed_buffer(ctx, ptr->len + 1);
    memcpy(duk_ptr, ev_data, ptr->len);
  }

  // call the callback
  duk_int_t rc = duk_pcall(ctx, 2);
  if (rc != 0) {
    mos_duk_log_error(ctx);
  }
  duk_pop(ctx);
}

static duk_ret_t mos_duk_func__event_on(duk_context* ctx) {
  // 0 = single event
  // 1 = event group
  duk_uint_t flag = (duk_uint_t) duk_get_current_magic(ctx);
  // fail early if invalid event is given
  int event_number = duk_require_int(ctx, 0);
  
  char id_buf[7] = {0};
  char callback_id[10] = "ecb";
  gen_random_id(id_buf, 6);
  strcat(callback_id, id_buf);
  callback_id[9] = '\0';
  
  duk_require_function(ctx, 1);
  duk_dup(ctx, 1); // store js callback
  duk_put_global_string(ctx, callback_id); // assign callback to a global var
  LOG(LL_DEBUG, ("Registering event id %d with callback as: %s", event_number, callback_id));

  mgosEventCallback* eventCallbackData = malloc(sizeof(mgosEventCallback));
  if (eventCallbackData == NULL) {
    return DUK_RET_RANGE_ERROR;
  }

  eventCallbackData->ctx = ctx;
  strcpy(eventCallbackData->id, callback_id);
  bool ret;
  if (flag == 1) {
    ret = mgos_event_add_group_handler(event_number, mos_duk_event_cb_handler, (void *) eventCallbackData);
  } else {
    ret = mgos_event_add_handler(event_number, mos_duk_event_cb_handler, (void *) eventCallbackData);
  }
  duk_push_boolean(ctx, ret);
  return 1;
}

static duk_ret_t mos_duk_func__gpio_set_mode(duk_context* ctx) {
  int pin = duk_require_int(ctx, 0);
  int mode = duk_require_int(ctx, 1);
  
  bool res = mgos_gpio_set_mode(pin, mode);
  duk_push_boolean(ctx, res);
  return 1;
}

static duk_ret_t mos_duk_func__gpio_write(duk_context* ctx) {
  int pin = duk_require_int(ctx, 0);
  bool level = duk_require_boolean(ctx, 1);
  
  mgos_gpio_write(pin, level);
  return 0;
}

static duk_ret_t mos_duk_func__gpio_read(duk_context* ctx) {
  int pin = duk_require_int(ctx, 0);
  
  bool res = mgos_gpio_read(pin);
  duk_push_boolean(ctx, res);
  return 1;
}

static duk_ret_t mos_duk_func__sys_heap_size(duk_context* ctx) {
  duk_push_int(ctx, mgos_get_heap_size());
  return 1;
}

static duk_ret_t mos_duk_func__sys_free_heap_size(duk_context* ctx) {
  duk_push_int(ctx, mgos_get_free_heap_size());
  return 1;
}

static duk_ret_t mos_duk_func__sys_min_free_heap_size(duk_context* ctx) {
  duk_push_int(ctx, mgos_get_min_free_heap_size());
  return 1;
}

// NOTE: mgos_get_fs_memory_usage is undefined on esp32
// static duk_ret_t mos_duk_func__sys_fs_mem_usage(duk_context* ctx) {
//   duk_push_int(ctx, mgos_get_fs_memory_usage());
//   return 1;
// }

static duk_ret_t mos_duk_func__sys_fs_size(duk_context* ctx) {
  duk_push_int(ctx, mgos_get_fs_size());
  return 1;
}

static duk_ret_t mos_duk_func__sys_fs_free_usage(duk_context* ctx) {
  duk_push_int(ctx, mgos_get_free_fs_size());
  return 1;
}

static duk_ret_t mos_duk_func__sys_fs_gc(duk_context* ctx) {
  mgos_fs_gc();
  return 0;
}

static duk_ret_t mos_duk_func__sys_fs_wdt_feed(duk_context* ctx) {
  mgos_wdt_feed();
  return 0;
}

static duk_ret_t mos_duk_func__sys_wdt_set_timeout(duk_context* ctx) {
  int timeout = duk_require_int(ctx, 0);

  mgos_wdt_set_timeout(timeout);
  return 0;
}

static duk_ret_t mos_duk_func__sys_wdt(duk_context* ctx) {
  bool enabled = duk_require_boolean(ctx, 0);
  if (enabled) {
    mgos_wdt_enable();
  } else {
    mgos_wdt_disable();
  }

  return 0;
}

static duk_ret_t mos_duk_func__sys_restart(duk_context* ctx) {
  mgos_system_restart();
  return 0;
}

static duk_ret_t mos_duk_func__mos_time_set(duk_context* ctx) {
  duk_int_t top = duk_get_top(ctx);
  if (top > 2 || top < 1) {
    duk_push_error_object(ctx, DUK_ERR_TYPE_ERROR, "At least one parameter is needed. Second parameter is timezone.");
    return 1;
  }

  double time = duk_require_number(ctx, 0);
  if (top == 2) {
    duk_push_error_object(ctx, DUK_ERR_TYPE_ERROR, "Timezone parsing is still #TODO#. Sorry.");
    return 1;
  }

  mgos_settimeofday(time, NULL);
  return 0;
}

void mos_duk_define_functions(duk_context* ctx) {
  // print(...)
  duk_push_c_function(ctx, &mos_duk_func_native_print, DUK_VARARGS);
  duk_put_global_string(ctx, "print");

  // console
  duk_push_global_object(ctx);
  mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "assert", LOG_ASSERT);
	mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "log", LOG_INFO);
  mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "dir", LOG_INFO);
  mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "info", LOG_INFO);
	mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "debug", LOG_DEBUG);
  mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "error", LOG_ERROR);
  mos_duk_reg_vararg_func(ctx, mos_duk_func_log, "warn", LOG_WARN);
  duk_put_global_string(ctx, "console");

  // global utils
  ADD_GLOBAL_FUNCTION("setInterval", mos_duk_func__set_interval, 2);
  ADD_GLOBAL_FUNCTION("setTimeout", mos_duk_func__set_timeout, 2);
  // TODO: setTimeout

  // MOS
  duk_push_object(ctx);
  // MOS ADC
  duk_push_object(ctx); // MOS.ADC
  ADD_FUNCTION("enable", mos_duk_func__adc_enable, 1);
  ADD_FUNCTION("read", mos_duk_func__adc_read, 1);
  duk_put_prop_string(ctx, -2, "ADC");
  // MOS BitBang
  duk_push_object(ctx); // MOS.BitBang
#if MGOS_ENABLE_BITBANG
  ADD_BOOLEAN("enabled", true);
  ADD_INT("MGOS_DELAY_MSEC", MGOS_DELAY_MSEC);
  ADD_INT("MGOS_DELAY_USEC", MGOS_DELAY_USEC);
  ADD_INT("MGOS_DELAY_100NSEC", MGOS_DELAY_100NSEC);
  ADD_FUNCTION("write", mos_duk_func__bitbang_write, 7);
#else
  ADD_BOOLEAN("enabled", false);
#endif
  duk_put_prop_string(ctx, -2, "BitBang");
  // MOS Config
  duk_push_object(ctx); // MOS.Config
  ADD_FUNCTION("get", mos_duk_func__config_get, 1);
  ADD_FUNCTION("set", mos_duk_func__config_set, 2);
  ADD_INT("MGOS_CONFIG_LEVEL_DEFAULTS", MGOS_CONFIG_LEVEL_DEFAULTS);
  ADD_INT("MGOS_CONFIG_LEVEL_VENDOR_1", MGOS_CONFIG_LEVEL_VENDOR_1);
  ADD_INT("MGOS_CONFIG_LEVEL_VENDOR_2", MGOS_CONFIG_LEVEL_VENDOR_2);
  ADD_INT("MGOS_CONFIG_LEVEL_VENDOR_3", MGOS_CONFIG_LEVEL_VENDOR_3);
  ADD_INT("MGOS_CONFIG_LEVEL_VENDOR_4", MGOS_CONFIG_LEVEL_VENDOR_4);
  ADD_INT("MGOS_CONFIG_LEVEL_VENDOR_5", MGOS_CONFIG_LEVEL_VENDOR_5);
  ADD_INT("MGOS_CONFIG_LEVEL_VENDOR_6", MGOS_CONFIG_LEVEL_VENDOR_6);
  ADD_INT("MGOS_CONFIG_LEVEL_VENDOR_7", MGOS_CONFIG_LEVEL_VENDOR_7);
  ADD_INT("MGOS_CONFIG_LEVEL_VENDOR_8", MGOS_CONFIG_LEVEL_VENDOR_8);
  ADD_INT("MGOS_CONFIG_LEVEL_USER", MGOS_CONFIG_LEVEL_USER);
  ADD_FUNCTION("reset", mos_duk_func__config_reset, 1);
  duk_put_prop_string(ctx, -2, "Config");
  // MOS Event
  duk_push_object(ctx); // MOS.Event
  ADD_INT("MGOS_EVENT_SYS", MGOS_EVENT_SYS);
  ADD_INT("MGOS_EVENT_INIT_DONE", MGOS_EVENT_INIT_DONE);
  ADD_INT("MGOS_EVENT_LOG", MGOS_EVENT_LOG);
  ADD_INT("MGOS_EVENT_REBOOT", MGOS_EVENT_REBOOT);
  ADD_INT("MGOS_EVENT_TIME_CHANGED", MGOS_EVENT_TIME_CHANGED);
  ADD_INT("MGOS_EVENT_CLOUD_CONNECTED", MGOS_EVENT_CLOUD_CONNECTED);
  ADD_INT("MGOS_EVENT_CLOUD_DISCONNECTED", MGOS_EVENT_CLOUD_DISCONNECTED);
  ADD_INT("MGOS_EVENT_CLOUD_CONNECTING", MGOS_EVENT_CLOUD_CONNECTING);
  ADD_INT("MGOS_EVENT_REBOOT_AFTER", MGOS_EVENT_REBOOT_AFTER);
  ADD_FUNCTION("register", mos_duk_func__event_register, 2);
  ADD_FUNCTION("baseNumber", mos_duk_func__event_base_number, 1);
  ADD_FUNCTION("trigger", mos_duk_func__event_trigger, DUK_VARARGS);
  ADD_FUNCTION_W_FLAG("on", mos_duk_func__event_on, 2 /* argc */, 0 /* flag */);
  ADD_FUNCTION_W_FLAG("onGroup", mos_duk_func__event_on, 2 /* argc */, 1 /* flag */);
  // TODO: how can we remove an event?
  duk_put_prop_string(ctx, -2, "Event");
  // MOS GPIO
  duk_push_object(ctx); // MOS.GPIO
  ADD_INT("MGOS_GPIO_MODE_INPUT", MGOS_GPIO_MODE_INPUT);
  ADD_INT("MGOS_GPIO_MODE_OUTPUT", MGOS_GPIO_MODE_OUTPUT);
  ADD_INT("MGOS_GPIO_PULL_NONE", MGOS_GPIO_PULL_NONE);
  ADD_INT("MGOS_GPIO_PULL_UP", MGOS_GPIO_PULL_UP);
  ADD_INT("MGOS_GPIO_PULL_DOWN", MGOS_GPIO_PULL_DOWN);
  ADD_FUNCTION("register", mos_duk_func__gpio_set_mode, 2);
  ADD_FUNCTION("write", mos_duk_func__gpio_write, 2);
  ADD_FUNCTION("read", mos_duk_func__gpio_read, 1);
  // TODO: Interrupts and mgos_gpio_set_button_handler
  //       Since they are ran inside an interrupt handler,
  //       running js is not safe. We should have a flag
  //       to evaluate JS after an interrupt occurs.
  duk_put_prop_string(ctx, -2, "GPIO");
  // TODO: write I2C handlers
  // TODO write NET handlers
  // TODO write OneWire handlers
  // TODO write PWM handlers
  // TODO write SPI handlers
  // MOS System
  duk_push_object(ctx); // MOS.System
  ADD_FUNCTION("heapSize", mos_duk_func__sys_heap_size, 0);
  ADD_FUNCTION("freeHeapSize", mos_duk_func__sys_free_heap_size, 0);
  ADD_FUNCTION("minFreeHeapSize", mos_duk_func__sys_min_free_heap_size, 0);
  // ADD_FUNCTION("fsMemUsage", mos_duk_func__sys_fs_mem_usage, 0); // not available on esp32
  ADD_FUNCTION("fsSize", mos_duk_func__sys_fs_size, 0);
  ADD_FUNCTION("freeFsSize", mos_duk_func__sys_fs_free_usage, 0);
  ADD_FUNCTION("fsGC", mos_duk_func__sys_fs_gc, 0);
  ADD_FUNCTION("wdtFeed", mos_duk_func__sys_fs_wdt_feed, 0);
  ADD_FUNCTION("wdtSetTimeout", mos_duk_func__sys_wdt_set_timeout, 1);
  ADD_FUNCTION("wdt", mos_duk_func__sys_wdt, 1);
  ADD_FUNCTION("restart", mos_duk_func__sys_restart, 0);
  // TODO: locks, enable/disable interrupts and sleep
  duk_put_prop_string(ctx, -2, "System");
  // MOS Time
  duk_push_object(ctx); // MOS.Time
  ADD_FUNCTION("uptime", mos_duk_func__mos_timers_uptime, 1);
  ADD_FUNCTION("set", mos_duk_func__mos_time_set, DUK_VARARGS);
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