/*
  +----------------------------------------------------------------------+
  | PHP Version 7                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2016 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: maben <www.maben@foxmail.com>                                |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_cfc.h"
#include "zend_smart_str.h"
#include <hiredis/hiredis.h>
#include "log.h"
#include <pthread.h>
#include <fcntl.h>

#define HASH_TABLE_NAME "cfc_hash"

static int module_is_shutdown = 0;

/* If you declare any globals in php_cfc.h uncomment this:
ZEND_DECLARE_MODULE_GLOBALS(cfc)
*/
static redisContext *g_redis = NULL;

static void (*old_zend_execute_ex)(zend_execute_data *execute_data);

/* True global resources - no need for thread safety here */
static int   le_cfc;
static int   cfc_enable       =   0;
static char* cfc_redis_host   =   NULL;
static int   cfc_redis_port   =   6379;
static char* cfc_prefix       =   NULL;
static char* cfc_logfile      =   NULL;

static cfc_manager_t  __manager, *manager_ptr = &__manager;

pthread_t worker_tid = 0;

typedef struct {
    char *orig;
    int count;
    char **val;
} cfc_split_t;

cfc_split_t cfc_prefixs = { 0 };

static int cfc_split(char *delim, char *str, cfc_split_t *t)
{
    int len = strlen(str);
    t->orig = (char *)malloc(strlen(str) * sizeof(char) + 1);
    t->count = 0;
    memcpy(t->orig, str, len);
    t->orig[len] = '\0';
    char *p = strtok(t->orig, delim);
    while (p) {
        if (t->count == 0) {
            t->val = (char **)malloc(sizeof(char *));
        } else {
            t->val = (char **)realloc(t->val, sizeof(char *) * t->count);
        }
        t->val[t->count] = strdup(p);
        p = strtok(NULL, delim);
        t->count++;
    }
    return 0;
}

static void cfc_split_free(cfc_split_t *t)
{
	free(t->orig);
	for (int i = 0; i < t->count; i++) {
		free(t->val[0]);
	}
	if (t->count > 0) {
		free(t->val);
	}
}


/* {{{ PHP_INI
 */
/* Remove comments and fill if you need to have entries in php.ini
PHP_INI_BEGIN()
    STD_PHP_INI_ENTRY("cfc.enable",      "1", PHP_INI_ALL, OnUpdateBool, global_value, zend_cfc_globals, cfc_globals)
    STD_PHP_INI_ENTRY("cfc.redis_host", "127.0.0.1", PHP_INI_ALL, OnUpdateString, redis_host, zend_cfc_globals, cfc_globals)
    STD_PHP_INI_ENTRY("cfc.redis_port", "6379", PHP_INI_ALL, OnUpdateLong, redis_port, zend_cfc_globals, cfc_globals)
PHP_INI_END()
*/
void cfc_atoi(const char *str, int *ret, int *len)
{
    const char *ptr = str;
    char ch;
    int absolute = 1;
    int rlen, result;

    ch = *ptr;

    if (ch == '-') {
        absolute = -1;
        ++ptr;
    } else if (ch == '+') {
        absolute = 1;
        ++ptr;
    }

    for (rlen = 0, result = 0; *ptr != '\0'; ptr++) {
        ch = *ptr;

        if (ch >= '0' && ch <= '9') {
            result = result * 10 + (ch - '0');
            rlen++;
        } else {
            break;
        }
    }

    if (ret) *ret = absolute * result;
    if (len) *len = rlen;
}

ZEND_INI_MH(php_cfc_enable)
{
    if (!new_value || new_value->len == 0) {
        return FAILURE;
    }

    if (!strcasecmp(new_value->val, "on") || !strcmp(new_value->val, "1")) {
        cfc_enable = 1;
    } else {
        cfc_enable = 0;
    }

    return SUCCESS;
}

ZEND_INI_MH(php_cfc_redis_host)
{
    if (!new_value || new_value->len == 0) {
        return FAILURE;
    }

    cfc_redis_host = strdup(new_value->val);
    if (cfc_redis_host == NULL) {
        return FAILURE;
    }

    return SUCCESS;
}

ZEND_INI_MH(php_cfc_redis_port)
{
    int len;

    if (!new_value || new_value->len == 0) {
        return FAILURE;
    }

    cfc_atoi(new_value->val, &cfc_redis_port, &len);

    if (len == 0) { /*failed */
        return FAILURE;
    }

    return SUCCESS;
}

ZEND_INI_MH(php_cfc_prefix)
{
    if (!new_value || new_value->len == 0) {
        return FAILURE;
    }
    cfc_prefix = strdup(new_value->val);
    if (cfc_prefix == NULL) {
        return FAILURE;
    }

    return SUCCESS;
}

ZEND_INI_MH(php_cfc_logfile)
{
    if (!new_value || new_value->len == 0) {
        return FAILURE;
    }

    cfc_logfile = strdup(new_value->val);
    if (cfc_logfile== NULL) {
        return FAILURE;
    }

    return SUCCESS;
}

PHP_INI_BEGIN()
	PHP_INI_ENTRY("cfc.enable", "0", PHP_INI_ALL, php_cfc_enable)
	PHP_INI_ENTRY("cfc.redis_host", "127.0.0.1", PHP_INI_ALL, php_cfc_redis_host)
	PHP_INI_ENTRY("cfc.redis_port", "6379", PHP_INI_ALL, php_cfc_redis_port)
	PHP_INI_ENTRY("cfc.prefix", "", PHP_INI_ALL, php_cfc_prefix)
	PHP_INI_ENTRY("cfc.logfile", "/tmp/cfc.log", PHP_INI_ALL, php_cfc_logfile)
PHP_INI_END()
/* }}} */

/* Remove the following function when you have successfully modified config.m4
   so that your module can be compiled into PHP, it exists only for testing
   purposes. */

/* Every user-visible function in PHP should document itself in the source */
/* {{{ proto string confirm_cfc_compiled(string arg)
   Return a string to confirm that the module is compiled in */

int redis_init()
{
    char *msg;

    if (!cfc_redis_host) {
        CFC_LOG_ERROR("redis host have not set");
		return -1;
    }

    g_redis = redisConnect(cfc_redis_host, cfc_redis_port);
    if (g_redis == NULL || g_redis->err) {
        CFC_LOG_ERROR("Can not connect to redis server");
		return -1;
    }
    return 0;
}

void redis_free()
{
	redisFree(g_redis);
}

int redis_incr(char *func)
{
	int r = -1;

    redisReply *reply = NULL;
    smart_str command = { 0 };
    smart_str_appends(&command, "HINCRBY ");
    smart_str_appendl(&command, HASH_TABLE_NAME, strlen(HASH_TABLE_NAME));
    smart_str_appendl(&command, " ", strlen(" "));
    smart_str_appendl(&command, func, strlen(func));
    smart_str_appendl(&command, " 1", strlen(" 1"));
    smart_str_0(&command);
    reply = redisCommand(g_redis, command.s->val);
    if (g_redis->err != 0) {
        CFC_LOG_ERROR("redis hash set failure, error:%d, command:%s\n", g_redis->err, command.s);
    } else {
        r = (int)reply->integer;
    }
    smart_str_free(&command);
    freeReplyObject(reply);
}

PHP_FUNCTION(confirm_cfc_compiled)
{
	char *arg = NULL;
	size_t arg_len, len;
	zend_string *strg;

	if (zend_parse_parameters(ZEND_NUM_ARGS(), "s", &arg, &arg_len) == FAILURE) {
		return;
	}

	strg = strpprintf(0, "Congratulations! You have successfully modified ext/%.78s/config.m4. Module %.78s is now compiled into PHP.", "cfc", arg);

	RETURN_STR(strg);
}
/* }}} */
/* The previous line is meant for vim and emacs, so it can correctly fold and
   unfold functions in source code. See the corresponding marks just before
   function definition, where the functions purpose is also documented. Please
   follow this convention for the convenience of others editing your code.
*/


/* {{{ php_cfc_init_globals
 */
/* Uncomment this function if you have INI entries
static void php_cfc_init_globals(zend_cfc_globals *cfc_globals)
{
	cfc_globals->global_value = 0;
	cfc_globals->global_string = NULL;
}
*/
/* }}} */

int set_nonblocking(int fd)
{
    int flags;

    if ((flags = fcntl(fd, F_GETFL, 0)) < 0 ||
         fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return -1;
    return 0;
}

static char *get_function_name(zend_execute_data * execute_data)
{
	zend_execute_data *data;
	char *ret = NULL;
	int len;
	const char * cls;
	const char * func;
	zend_function *curr_func;
	uint32_t curr_op;
	const zend_op *opline;

	data = EG(current_execute_data);

	if (data)
	{
		curr_func = data->func;
		/* extract function name from the meta info */
		if (curr_func->common.function_name)
		{
			func = curr_func->common.function_name->val;
			len  = curr_func->common.function_name->len + 1;
			cls = curr_func->common.scope ?
					curr_func->common.scope->name->val :
					(data->called_scope ?
							data->called_scope->name->val : NULL);
			if (cls)
			{
				len = strlen(cls) + strlen(func) + 10;
				ret = (char*) emalloc(len);
				snprintf(ret, len, "%s::%s", cls, func);
			}
			else
			{
				ret = (char*) emalloc(len);
				snprintf(ret, len, "%s", func);
			}
		}
		else
		{
			if (data->prev_execute_data)
			{
				opline  = data->prev_execute_data->opline;
			}
			else
			{
				opline  = data->opline;
			}
			switch (opline->extended_value)
			{
			case ZEND_EVAL:
				func = "eval";
				break;
			default:
				func = NULL;
				break;
			}

			if (func)
			{
				ret = estrdup(func);
			}
		}
	}
	return ret;
}

static void push_func_to_queue(char *func)
{
	if (NULL == func) {
		return;
	}
	cfc_item_t *item;
	int length = 0;
	length = sizeof(*item) + strlen (func) + 1;
	item = (cfc_item_t *)emalloc(length);

	if (!item) {
		return;
	}
	item->size = strlen(func);
	item->next = NULL;
	memcpy(item->buffer, func, strlen(func));

	spin_lock(&manager_ptr->qlock);

	if (!manager_ptr->head) {
		manager_ptr->head = item;
	}

	if (manager_ptr->tail) {
		manager_ptr->tail->next = item;
	}

	manager_ptr->tail = item;

	spin_unlock(&manager_ptr->qlock);

	/* notify worker thread */
	write(manager_ptr->notifiers[1], "\0", 1);
}

static void my_zend_execute_ex(zend_execute_data *execute_data)
{
	char *func = NULL;
	func = get_function_name(execute_data TSRMLS_CC);
	if (!func) {
		goto end;
	}
	if (cfc_prefixs.count) {
		for (int i = 0; i < cfc_prefixs.count; i++) {
			if (strncmp(cfc_prefixs.val[i], func, strlen(cfc_prefixs.val[i])) == 0) {
				push_func_to_queue(func);
			}
		}
	} else {
		push_func_to_queue(func);
	}
	efree(func);
end:
	old_zend_execute_ex(execute_data TSRMLS_CC);
}

void *cfc_thread_worker(void *arg)
{
	fd_set read_set;
	int notify = manager_ptr->notifiers[0];
	struct timeval tv = {10, 0}; /* 1 sec to update redis */

	FD_ZERO(&read_set);

	for (;;) {

		if (module_is_shutdown) {
			break;
		}

		FD_SET(notify, &read_set);

		(void)select(notify + 1, &read_set, NULL, NULL, &tv);

		if (FD_ISSET(notify, &read_set)) {
			char tmp;
			int result;
			cfc_item_t *item;

			for (;;) {
				result = read(notify, &tmp, 1);
				if (result == -1 || tmp != '\0') {
					break;
				}

				/* Get item from worker queue */

				spin_lock(&manager_ptr->qlock);

				item = manager_ptr->head;

				if (item) {
					manager_ptr->head = item->next;
				} else {
					manager_ptr->head = NULL;
				}

				if (!manager_ptr->head) {
					manager_ptr->tail = NULL;
				}

				spin_unlock(&manager_ptr->qlock);

				if (item) {
					item->buffer[item->size] = '\0';
					result = redis_incr(item->buffer);
					efree(item);
				}
			}
		}
	}

	return NULL;
}

int cfc_init(void)
{
	manager_ptr->head = NULL;
	manager_ptr->tail = NULL;
	manager_ptr->qlock = 0; /* init can lock */

	if (pipe(manager_ptr->notifiers) == -1) {
		return -1;
	}

	if (cfc_init_log(cfc_logfile, CFC_LOG_LEVEL_DEBUG) == -1) {
		close(manager_ptr->notifiers[0]);
		close(manager_ptr->notifiers[1]);
		return -1;
	}

	set_nonblocking(manager_ptr->notifiers[0]);
	set_nonblocking(manager_ptr->notifiers[1]);

	if (pthread_create(&worker_tid, NULL,
		cfc_thread_worker, NULL) == -1)
	{
		close(manager_ptr->notifiers[0]);
		close(manager_ptr->notifiers[1]);
		return -1;
	}


	if (redis_init() == -1) {
		return -1;
	}

	return 0;

}
/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(cfc)
{
	REGISTER_INI_ENTRIES();
	if (!cfc_enable) {
		return SUCCESS;
	}
	if (cfc_init() == -1) {
		return FAILURE;
	}
	cfc_split(",", cfc_prefix, &cfc_prefixs);
	old_zend_execute_ex = zend_execute_ex;
	zend_execute_ex = my_zend_execute_ex;
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(cfc)
{
	module_is_shutdown = 1;
	UNREGISTER_INI_ENTRIES();
	if (!cfc_enable) {
		return SUCCESS;
	}
	if (worker_tid) {
		pthread_join(worker_tid, NULL);
	}
	redis_free();
	cfc_split_free(&cfc_prefixs);
	zend_execute_ex = old_zend_execute_ex;
	return SUCCESS;
}

/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(cfc)
{
#if defined(COMPILE_DL_CFC) && defined(ZTS)
	ZEND_TSRMLS_CACHE_UPDATE();
#endif
	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(cfc)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(cfc)
{
	php_info_print_table_start();
	php_info_print_table_header(2, "cfc support", "enabled");
	php_info_print_table_end();

	/* Remove comments if you have entries in php.ini
	DISPLAY_INI_ENTRIES();
	*/
}
/* }}} */

/* {{{ cfc_functions[]
 *
 * Every user visible function must have an entry in cfc_functions[].
 */
const zend_function_entry cfc_functions[] = {
	PHP_FE(confirm_cfc_compiled,	NULL)		/* For testing, remove later. */
	PHP_FE_END	/* Must be the last line in cfc_functions[] */
};
/* }}} */

/* {{{ cfc_module_entry
 */
zend_module_entry cfc_module_entry = {
	STANDARD_MODULE_HEADER,
	"cfc",
	cfc_functions,
	PHP_MINIT(cfc),
	PHP_MSHUTDOWN(cfc),
	PHP_RINIT(cfc),		/* Replace with NULL if there's nothing to do at request start */
	PHP_RSHUTDOWN(cfc),	/* Replace with NULL if there's nothing to do at request end */
	PHP_MINFO(cfc),
	PHP_CFC_VERSION,
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_CFC
#ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE();
#endif
ZEND_GET_MODULE(cfc)
#endif

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
