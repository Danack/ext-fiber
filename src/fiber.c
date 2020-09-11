/*
  +--------------------------------------------------------------------+
  | ext-fiber                                                          |
  +--------------------------------------------------------------------+
  | Redistribution and use in source and binary forms, with or without |
  | modification, are permitted provided that the conditions mentioned |
  | in the accompanying LICENSE file are met.                          |
  +--------------------------------------------------------------------+
  | Authors: Martin Schröder <m.schroeder2007@gmail.com>               |
  |          Aaron Piotrowski <aaron@trowski.com>                      |
  +--------------------------------------------------------------------+
*/

#include "php.h"
#include "zend.h"
#include "zend_API.h"
#include "zend_vm.h"
#include "zend_interfaces.h"
#include "zend_exceptions.h"
#include "zend_closures.h"

#include "php_fiber.h"
#include "awaitable.h"
#include "fiber.h"

#ifndef ZEND_PARSE_PARAMETERS_NONE
#define ZEND_PARSE_PARAMETERS_NONE() zend_parse_parameters_none()
#endif

static zend_class_entry *zend_ce_fiber;
static zend_class_entry *zend_ce_fiber_error;
static zend_object_handlers zend_fiber_handlers;

static zend_object *zend_fiber_object_create(zend_class_entry *ce);
static void zend_fiber_object_destroy(zend_object *object);

static zend_op_array fiber_run_func;
static zend_try_catch_element fiber_terminate_try_catch_array = { 0, 1, 0, 0 };
static zend_op fiber_run_op[2];

static zend_string *fiber_continuation_name;

#define ZEND_FIBER_BACKUP_EG(stack, stack_page_size, exec) do { \
	stack = EG(vm_stack); \
	stack->top = EG(vm_stack_top); \
	stack->end = EG(vm_stack_end); \
	stack_page_size = EG(vm_stack_page_size); \
	exec = EG(current_execute_data); \
} while (0)

#define ZEND_FIBER_RESTORE_EG(stack, stack_page_size, exec) do { \
	EG(vm_stack) = stack; \
	EG(vm_stack_top) = stack->top; \
	EG(vm_stack_end) = stack->end; \
	EG(vm_stack_page_size) = stack_page_size; \
	EG(current_execute_data) = exec; \
} while (0)


static zend_bool zend_fiber_switch_to(zend_fiber *fiber)
{
	zend_fiber_context root;

	root = FIBER_G(root);

	if (root == NULL) {
		root = zend_fiber_create_root_context();

		if (root == NULL) {
			return 0;
		}

		FIBER_G(root) = root;
	}

	zend_fiber *prev;
	zend_bool result;
	zend_execute_data *exec;
	zend_vm_stack stack;
	size_t stack_page_size;

	ZEND_FIBER_BACKUP_EG(stack, stack_page_size, exec);

	prev = FIBER_G(current_fiber);
	FIBER_G(current_fiber) = fiber;

	result = zend_fiber_switch_context((prev == NULL) ? root : prev->context, fiber->context);

	FIBER_G(current_fiber) = prev;

	ZEND_FIBER_RESTORE_EG(stack, stack_page_size, exec);

	return result;
}


static void zend_fiber_run()
{
	zend_fiber *fiber;

	fiber = FIBER_G(current_fiber);
	ZEND_ASSERT(fiber != NULL);

	EG(vm_stack) = fiber->stack;
	EG(vm_stack_top) = fiber->stack->top;
	EG(vm_stack_end) = fiber->stack->end;
	EG(vm_stack_page_size) = ZEND_FIBER_VM_STACK_SIZE;

	fiber->exec = (zend_execute_data *) EG(vm_stack_top);
	EG(vm_stack_top) = (zval *) fiber->exec + ZEND_CALL_FRAME_SLOT;
	zend_vm_init_call_frame(fiber->exec, ZEND_CALL_TOP_FUNCTION, (zend_function *) &fiber_run_func, 0, NULL);
	fiber->exec->opline = fiber_run_op;
	fiber->exec->call = NULL;
	fiber->exec->return_value = NULL;
	fiber->exec->prev_execute_data = NULL;

	EG(current_execute_data) = fiber->exec;

	execute_ex(fiber->exec);

	zend_vm_stack_destroy();
	fiber->stack = NULL;
	fiber->exec = NULL;

	zend_fiber_suspend(fiber->context);

	abort();
}


static int fiber_invoke_callback(zend_fiber *fiber, zend_fcall_info *fci, zend_fcall_info_cache *fci_cache)
{
	zval retval;
	
	fci->params = fiber->result;
	fci->param_count = 2;
#if PHP_VERSION_ID < 80000
	fci->no_separation = 1;
#endif
	fci->retval = &retval;
	
	return zend_call_function(fci, fci_cache);
}


static void fiber_invoke_callbacks(zend_fiber *fiber)
{
	zend_awaitable_callback *callback;
	zend_object *error = NULL;
	zval exception;
	
	ZEND_HASH_FOREACH_PTR(fiber->callbacks, callback) {
		fiber_invoke_callback(fiber, &callback->fci, &callback->fci_cache);
		
		if (EG(exception)) {
			if (error != NULL) {
				zend_exception_set_previous(EG(exception), error);
				GC_DELREF(error);
			}
			
			error = EG(exception);
			GC_ADDREF(error);
			
			zend_clear_exception();
		}
	} ZEND_HASH_FOREACH_END();
	
	if (error != NULL) {
		ZVAL_OBJ(&exception, error);
		Z_ADDREF(exception);
		zend_throw_exception_object(&exception);
		zval_ptr_dtor(&exception);

		zend_throw_error(NULL, "Exception thrown in when() callback");
	}
}


static void delete_awaitable_callback(zval *ptr)
{
	zend_awaitable_callback *callback = Z_PTR_P(ptr);
	zval_ptr_dtor(&callback->fci.function_name);
	efree(callback);
}


static int fiber_run_opcode_handler(zend_execute_data *exec)
{
	zend_fiber *fiber;
	zval retval;

	fiber = FIBER_G(current_fiber);
	ZEND_ASSERT(fiber != NULL);

	fiber->status = ZEND_FIBER_STATUS_RUNNING;
	fiber->fci.retval = &retval;

	zend_call_function(&fiber->fci, &fiber->fci_cache);
	
	zval_ptr_dtor(&fiber->fci.function_name);
	GC_DELREF(&fiber->std);

	if (EG(exception)) {
		fiber->status = ZEND_FIBER_STATUS_DEAD;
		ZVAL_OBJ(&fiber->result[0], EG(exception));
		Z_ADDREF(fiber->result[0]);
		zend_clear_exception();
	} else {
		fiber->status = ZEND_FIBER_STATUS_FINISHED;
		ZVAL_COPY_VALUE(&fiber->result[1], &retval);
	}
	
	fiber_invoke_callbacks(fiber);
	
	zend_hash_destroy(fiber->callbacks);
	FREE_HASHTABLE(fiber->callbacks);
	fiber->callbacks = NULL;

	return ZEND_USER_OPCODE_RETURN;
}


static zend_object *zend_fiber_object_create(zend_class_entry *ce)
{
	zend_fiber *fiber;

	fiber = emalloc(sizeof(zend_fiber));
	memset(fiber, 0, sizeof(zend_fiber));

	zend_object_std_init(&fiber->std, ce);
	fiber->std.handlers = &zend_fiber_handlers;
	
	ZVAL_NULL(&fiber->result[0]);
	ZVAL_NULL(&fiber->result[1]);
	fiber->callbacks = zend_new_array(0);
	zend_hash_init(fiber->callbacks, 0, NULL, delete_awaitable_callback, 0);

	return &fiber->std;
}


static void zend_fiber_object_destroy(zend_object *object)
{
	zend_fiber *fiber;

	fiber = (zend_fiber *) object;

	if (fiber->status == ZEND_FIBER_STATUS_SUSPENDED) {
		fiber->status = ZEND_FIBER_STATUS_DEAD;

		zend_fiber_switch_to(fiber);
	}
	
	if (fiber->callbacks != NULL) {
		zend_hash_destroy(fiber->callbacks);
		FREE_HASHTABLE(fiber->callbacks);
		fiber->callbacks = NULL;
	}
	
	zval_ptr_dtor(&fiber->result[0]);
	zval_ptr_dtor(&fiber->result[1]);

	zend_fiber_destroy(fiber->context);

	zend_object_std_dtor(&fiber->std);
}


static ZEND_COLD zend_function *zend_fiber_get_constructor(zend_object *object)
{
	zend_throw_error(NULL, "Use Fiber::run() to create a new fiber");
	
	return NULL;
}


/* {{{ proto void Fiber::run(callable $callback, mixed ...$args) */
ZEND_METHOD(Fiber, run)
{
	zend_fiber *fiber;
	zval *params;
	uint32_t param_count;

	fiber = (zend_fiber *) zend_fiber_object_create(zend_ce_fiber);

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, -1)
		Z_PARAM_FUNC_EX(fiber->fci, fiber->fci_cache, 1, 0)
		Z_PARAM_VARIADIC('+', params, param_count)
	ZEND_PARSE_PARAMETERS_END();
	
	// Keep a reference to closures or callable objects as long as the fiber lives.
	Z_TRY_ADDREF(fiber->fci.function_name);
	
	fiber->fci.params = params;
	fiber->fci.param_count = param_count;
#if PHP_VERSION_ID < 80000
	fiber->fci.no_separation = 1;
#endif
	
	fiber->context = zend_fiber_create_context();
	fiber->stack_size = FIBER_G(stack_size);

	if (fiber->context == NULL) {
		zend_throw_error(NULL, "Failed to create native fiber context");
		return;
	}

	if (!zend_fiber_create(fiber->context, zend_fiber_run, fiber->stack_size)) {
		zend_throw_error(NULL, "Failed to create native fiber");
		return;
	}

	fiber->stack = (zend_vm_stack) emalloc(ZEND_FIBER_VM_STACK_SIZE);
	fiber->stack->top = ZEND_VM_STACK_ELEMENTS(fiber->stack) + 1;
	fiber->stack->end = (zval *) ((char *) fiber->stack + ZEND_FIBER_VM_STACK_SIZE);
	fiber->stack->prev = NULL;
	
	fiber->status = ZEND_FIBER_STATUS_RUNNING;
	
	if (!zend_fiber_switch_to(fiber)) {
		zend_throw_error(NULL, "Failed switching to fiber");
		return;
	}
	
	GC_ADDREF(&fiber->std);
	RETURN_OBJ(&fiber->std);
}
/* }}} */


/* {{{ proto void Fiber::continue() */
ZEND_METHOD(Fiber, continue)
{
	zend_fiber *fiber;
	zval *value = NULL;
	zval *exception = NULL;

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 2)
		Z_PARAM_OBJECT_OF_CLASS_EX(exception, zend_ce_throwable, 1, 0)
		Z_PARAM_OPTIONAL
		Z_PARAM_ZVAL(value);
	ZEND_PARSE_PARAMETERS_END();

	fiber = (zend_fiber *) Z_OBJ_P(getThis());
	
	if (fiber->status != ZEND_FIBER_STATUS_SUSPENDED) {
		zend_throw_error(zend_ce_fiber_error, "Cannot resume running fiber");
		return;
	}
	
	if (exception != NULL) {
		Z_ADDREF_P(exception);
		fiber->error = exception;
	} else if (value != NULL) {
		Z_TRY_ADDREF_P(value);
		fiber->value = value;
	}

	if (fiber->state == ZEND_FIBER_STATE_SUSPENDING) {
		fiber->state = ZEND_FIBER_STATE_READY;
		return;
	}
	
	fiber->status = ZEND_FIBER_STATUS_RUNNING;
	
	if (!zend_fiber_switch_to(fiber)) {
		zend_throw_error(NULL, "Failed switching to fiber");
		return;
	}
}
/* }}} */


/* {{{ proto Fiber::inFiber() */
ZEND_METHOD(Fiber, inFiber)
{
	ZEND_PARSE_PARAMETERS_NONE();
	
	if (FIBER_G(current_fiber) == NULL) {
		RETURN_FALSE;
	}
	
	RETURN_TRUE;
}
/* }}} */


/* {{{ proto void Fiber::await() */
ZEND_METHOD(Fiber, await)
{
	zend_fiber *fiber;
	zend_execute_data *exec;
	size_t stack_page_size;
	
	zval *awaitable;
	zval method_name;
	zval retval;
	
	zend_function *func;
	zval callback;
	zval context;
	int result;

	zval *error;
	
	fiber = FIBER_G(current_fiber);

	if (UNEXPECTED(fiber == NULL)) {
		zend_throw_error(zend_ce_fiber_error, "Cannot await from outside a fiber");
		return;
	}

	if (fiber->status != ZEND_FIBER_STATUS_RUNNING) {
		zend_throw_error(zend_ce_fiber_error, "Cannot await from a fiber that is not running");
		return;
	}

	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 1)
		Z_PARAM_OBJECT_OF_CLASS_EX(awaitable, zend_ce_awaitable, 0, 0)
	ZEND_PARSE_PARAMETERS_END();
	
	fiber->status = ZEND_FIBER_STATUS_SUSPENDED;
	
	ZVAL_OBJ(&context, &fiber->std);
	
	func = zend_hash_find_ptr(&zend_ce_fiber->function_table, fiber_continuation_name);
	zend_create_closure(&callback, func, zend_ce_fiber, zend_ce_fiber, &context);
	
	Z_DELREF(callback);
	
	fiber->state = ZEND_FIBER_STATE_SUSPENDING;
	
	ZVAL_STRING(&method_name, "when");
	result = call_user_function(NULL, awaitable, &method_name, &retval, 1, &callback);
	zval_ptr_dtor(&method_name);
	
	if (result == FAILURE) {
		fiber->state = ZEND_FIBER_STATE_READY;
		return;
	}

	if (fiber->state == ZEND_FIBER_STATE_SUSPENDING) {
		fiber->state = ZEND_FIBER_STATE_READY;
		
		ZEND_FIBER_BACKUP_EG(fiber->stack, stack_page_size, fiber->exec);

		zend_fiber_suspend(fiber->context);

		ZEND_FIBER_RESTORE_EG(fiber->stack, stack_page_size, fiber->exec);
		
		if (fiber->status == ZEND_FIBER_STATUS_DEAD) {
			zend_throw_error(zend_ce_fiber_error, "Fiber has been destroyed");
			return;
		}
	}

	if (fiber->error == NULL) {
		if (fiber->value != NULL) {
			ZVAL_COPY_VALUE(return_value, fiber->value);
			fiber->value = NULL;
		}
		return;
	}
	
	error = fiber->error;
	fiber->error = NULL;
	exec = EG(current_execute_data);

	exec->opline--;
	zend_throw_exception_object(error);
	exec->opline++;
}
/* }}} */


/* {{{ proto void Fiber::when(callable $when) */
ZEND_METHOD(Fiber, when)
{
	zend_fiber *fiber;
	zend_fcall_info fci;
	zend_fcall_info_cache fci_cache;
	zend_awaitable_callback *callback;
	
	ZEND_PARSE_PARAMETERS_START_EX(ZEND_PARSE_PARAMS_THROW, 1, 1)
		Z_PARAM_FUNC_EX(fci, fci_cache, 1, 0)
	ZEND_PARSE_PARAMETERS_END();
	
	fiber = (zend_fiber *) Z_OBJ_P(getThis());
	
	if (fiber->status == ZEND_FIBER_STATUS_FINISHED || fiber->status == ZEND_FIBER_STATUS_DEAD) {
		fiber_invoke_callback(fiber, &fci, &fci_cache);
		if (EG(exception)) {
			zend_throw_error(NULL, "Exception thrown in when callback");
		}
		return;
	}
	
	callback = emalloc(sizeof(zend_awaitable_callback));
	
	memcpy(&callback->fci, &fci, sizeof(zend_fcall_info));
	memcpy(&callback->fci_cache, &fci_cache, sizeof(zend_fcall_info_cache));
	
	Z_TRY_ADDREF(callback->fci.function_name);
	
	zend_hash_next_index_insert_ptr(fiber->callbacks, callback);
}
/* }}} */


/* {{{ proto FiberError::__construct(string $message) */
ZEND_METHOD(FiberError, __construct)
{
	zend_throw_error(NULL, "FiberError cannot be constructed manually");
}
/* }}} */


ZEND_BEGIN_ARG_WITH_RETURN_OBJ_INFO_EX(arginfo_fiber_run, 0, 1, Awaitable, 0)
	ZEND_ARG_CALLABLE_INFO(0, callable, 0)
	ZEND_ARG_VARIADIC_INFO(0, arguments)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fiber_inFiber, 0, 0, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fiber_continue, 0, 1, IS_VOID, 0)
	ZEND_ARG_OBJ_INFO(0, exception, Throwable, 1)
	ZEND_ARG_INFO(0, value)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fiber_when, 0, 0, IS_VOID, 0)
	ZEND_ARG_CALLABLE_INFO(0, when, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_INFO_EX(arginfo_fiber_await, 0, 0, 0)
	ZEND_ARG_OBJ_INFO(0, awaitable, Awaitable, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry fiber_methods[] = {
	ZEND_ME(Fiber, run, arginfo_fiber_run, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Fiber, continue, arginfo_fiber_continue, ZEND_ACC_PRIVATE)
	ZEND_ME(Fiber, when, arginfo_fiber_when, ZEND_ACC_PUBLIC)
	ZEND_ME(Fiber, inFiber, arginfo_fiber_inFiber, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_ME(Fiber, await, arginfo_fiber_await, ZEND_ACC_PUBLIC | ZEND_ACC_STATIC)
	ZEND_FE_END
};

ZEND_BEGIN_ARG_INFO(arginfo_fiber_error_create, 0)
	ZEND_ARG_TYPE_INFO(0, message, IS_STRING, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry fiber_error_methods[] = {
	ZEND_ME(FiberError, __construct, arginfo_fiber_error_create, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
	ZEND_FE_END
};

void zend_fiber_ce_register()
{
	zend_class_entry ce;
	zend_uchar opcode = ZEND_VM_LAST_OPCODE + 1;

	/* Create a new user opcode to run fiber. */
	while (1) {
		if (opcode == 255) {
			return;
		} else if (zend_get_user_opcode_handler(opcode) == NULL) {
			break;
		}
		opcode++;
	}

	zend_set_user_opcode_handler(opcode, fiber_run_opcode_handler);

	ZEND_SECURE_ZERO(fiber_run_op, sizeof(fiber_run_op));
	fiber_run_op[0].opcode = opcode;
	zend_vm_set_opcode_handler_ex(fiber_run_op, 0, 0, 0);
	fiber_run_op[1].opcode = opcode;
	zend_vm_set_opcode_handler_ex(fiber_run_op + 1, 0, 0, 0);

	ZEND_SECURE_ZERO(&fiber_run_func, sizeof(fiber_run_func));
	fiber_run_func.type = ZEND_USER_FUNCTION;
	fiber_run_func.function_name = zend_string_init("Fiber::run", sizeof("Fiber::run") - 1, 1);
	fiber_run_func.filename = ZSTR_EMPTY_ALLOC();
	fiber_run_func.opcodes = fiber_run_op;
	fiber_run_func.last_try_catch = 1;
	fiber_run_func.try_catch_array = &fiber_terminate_try_catch_array;

	INIT_CLASS_ENTRY(ce, "Fiber", fiber_methods);
	zend_ce_fiber = zend_register_internal_class(&ce);
	zend_ce_fiber->ce_flags |= ZEND_ACC_FINAL;
	zend_ce_fiber->create_object = zend_fiber_object_create;
	zend_ce_fiber->serialize = zend_class_serialize_deny;
	zend_ce_fiber->unserialize = zend_class_unserialize_deny;
	
	zend_class_implements(zend_ce_fiber, 1, zend_ce_awaitable);

	memcpy(&zend_fiber_handlers, &std_object_handlers, sizeof(zend_object_handlers));
	zend_fiber_handlers.free_obj = zend_fiber_object_destroy;
	zend_fiber_handlers.clone_obj = NULL;
	zend_fiber_handlers.get_constructor = zend_fiber_get_constructor;
	
	INIT_CLASS_ENTRY(ce, "FiberError", fiber_error_methods);
	zend_ce_fiber_error = zend_register_internal_class_ex(&ce, zend_ce_error);
	zend_ce_fiber_error->ce_flags |= ZEND_ACC_FINAL;
	zend_ce_fiber_error->create_object = zend_ce_error->create_object;
	
	fiber_continuation_name = zend_string_init("continue", sizeof("continue") - 1, 1);
}

void zend_fiber_ce_unregister()
{
	zend_string_free(fiber_run_func.function_name);
	fiber_run_func.function_name = NULL;
	zend_string_free(fiber_continuation_name);
}

void zend_fiber_shutdown()
{
	zend_fiber_context root;
	root = FIBER_G(root);

	FIBER_G(root) = NULL;

	zend_fiber_destroy(root);
}
