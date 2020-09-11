/*
  +--------------------------------------------------------------------+
  | ext-fiber                                                          |
  +--------------------------------------------------------------------+
  | Redistribution and use in source and binary forms, with or without |
  | modification, are permitted provided that the conditions mentioned |
  | in the accompanying LICENSE file are met.                          |
  +--------------------------------------------------------------------+
  | Authors: Aaron Piotrowski <aaron@trowski.com>                      |
  +--------------------------------------------------------------------+
*/

#include "php.h"
#include "zend.h"

#include "awaitable.h"

ZEND_API zend_class_entry *zend_ce_awaitable;

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_awaitable_onResolve, 0, 1, IS_VOID, 0)
	ZEND_ARG_CALLABLE_INFO(0, callable, 0)
ZEND_END_ARG_INFO()

static const zend_function_entry awaitable_methods[] = {
	ZEND_ABSTRACT_ME(Awaitable, onResolve, arginfo_awaitable_onResolve)
	ZEND_FE_END
};

void zend_awaitable_ce_register()
{
	zend_class_entry ce;
	
	INIT_CLASS_ENTRY(ce, "Awaitable", awaitable_methods);
	zend_ce_awaitable = zend_register_internal_interface(&ce);
}
