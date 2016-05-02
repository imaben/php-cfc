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

#define BUFFER_SIZE 1024

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
static char* cfc_ht_name      =   NULL;

static cfc_manager_t  __manager, *manager_ptr = &__manager;

pthread_t worker_tid = 0;
pthread_t queue_tid = 0;

static int stop_capture = 0;

typedef struct {
	char *orig;
	int count;
	char **val;
} cfc_split_t;

cfc_split_t cfc_prefixs = { 0 };

static int cfc_split(char *delim, char *str, cfc_split_t *t)
{
	int len = strlen(str);
	t->orig = (char *)pemalloc(strlen(str) * sizeof(char) + 1, 1);
	t->count = 0;
	memcpy(t->orig, str, len);
	t->orig[len] = '\0';
	char *p = strtok(t->orig, delim);
	while (p) {
		if (t->count == 0) {
			t->val = (char **)pemalloc(sizeof(char *), 1);
		} else {
			t->val = (char **)realloc(t->val, sizeof(char *) * (t->count + 1));
		}
		t->val[t->count] = strdup(p);
		p = strtok(NULL, delim);
		t->count++;
	}
	return 0;
}

static void cfc_split_free(cfc_split_t *t)
{
	pefree(t->orig, 1);
    int i;
	for (i = 0; i < t->count; i++) {
		pefree(t->val[i], 1);
	}
	if (t->count > 0) {
		pefree(t->val, 1);
	}
}

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

	if (ret)
		*ret = absolute * result;
	if (len)
		*len = rlen;
}

/* Remove the following function when you have successfully modified config.m4
   so that your module can be compiled into PHP, it exists only for testing
   purposes. */

/* Every user-visible function in PHP should document itself in the source */
/* {{{ proto string confirm_cfc_compiled(string arg)
   Return a string to confirm that the module is compiled in */

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
	reply = redisCommand(g_redis, "HINCRBY %s %s 1", cfc_ht_name, func);
	if (g_redis->err != 0
			|| reply == NULL
			|| reply->type != REDIS_REPLY_INTEGER) {
		CFC_LOG_ERROR("redis hash set failure, error:%d, errstr:%s\n", g_redis->err, g_redis->errstr);
	} else {
		r = (int)reply->integer;
	}

	if (g_redis->err == REDIS_ERR_EOF) { /** The server closed the connection */
		int retry = 0;
		while (redis_init() == -1) {
			if (retry > 3) {
				stop_capture = 1;
				break;
			}
			retry++;
			sleep(1);
		}
		if (retry <= 3) {
			freeReplyObject(reply);
			return redis_incr(func);
		}
	}
	freeReplyObject(reply);
	return r;
}

int set_nonblocking(int fd)
{
	int flags;

	if ((flags = fcntl(fd, F_GETFL, 0)) < 0 ||
			fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;
	return 0;
}

static char *get_function_name(zend_execute_data * execute_data, size_t *output_len)
{
	zend_execute_data *data;
	char *ret = NULL;
	size_t len;
	const char * cls;
	const char * func;
	zend_function *curr_func;
	uint32_t curr_op;
	const zend_op *opline;

	data = EG(current_execute_data);

	if (data) {
		curr_func = data->func;
		/* extract function name from the meta info */
		if (curr_func->common.function_name) {
			func = curr_func->common.function_name->val;
			len  = curr_func->common.function_name->len + 1;
			cls = curr_func->common.scope ? curr_func->common.scope->name->val :
					(data->called_scope ? data->called_scope->name->val : NULL);
			if (cls) {
				len = strlen(cls) + strlen(func) + strlen("::") + 1;
				ret = (char*) emalloc(len + sizeof(size_t));
				memcpy(ret, &len, sizeof(size_t));
				sprintf(ret + sizeof(size_t), "%s::%s", cls, func);
				*output_len = len + sizeof(size_t);
			} else {
				ret = (char*) emalloc(len + sizeof(size_t));
				memcpy(ret, &len, sizeof(size_t));
				sprintf(ret + sizeof(size_t), "%s", func);
				*output_len = len + sizeof(size_t);
			}
		} else {
			if (data->prev_execute_data) {
				opline  = data->prev_execute_data->opline;
			} else {
				opline  = data->opline;
			}

			switch (opline->extended_value) {
			case ZEND_EVAL:
				func = "eval";
				break;
			default:
				func = NULL;
				break;
			}

			if (func) {
				len = strlen(func) + 1;
				ret = (char*) emalloc(len + sizeof(size_t));
				memcpy(ret, &len, sizeof(size_t));
				memcpy(ret + sizeof(size_t), func, len);
			}
		}
	}
	return ret;
}

static void push_func_to_queue(char *func, size_t len)
{
	if (NULL == func) {
		return;
	}
	write(manager_ptr->queues[1], func, len);
}

static void my_zend_execute_ex(zend_execute_data *execute_data)
{
	if (stop_capture) {
		goto end;
	}
	char *func = NULL;
	size_t len;
    int i;
	func = get_function_name(execute_data, &len);
	if (!func) {
		goto end;
	}
	if (cfc_prefixs.count) {
		for (i = 0; i < cfc_prefixs.count; i++) {
			if (strncmp(cfc_prefixs.val[i], func + sizeof(size_t), strlen(cfc_prefixs.val[i])) == 0) {
				push_func_to_queue(func, len);
			}
		}
	} else {
		push_func_to_queue(func, len);
	}
	efree(func);
end:
	old_zend_execute_ex(execute_data TSRMLS_CC);
}

void *cfc_thread_worker(void *arg)
{
	fd_set read_set;
	int notify = manager_ptr->notifiers[0];

	for (;;) {

		FD_ZERO(&read_set);
		FD_SET(notify, &read_set);

		int rst = select(notify + 1, &read_set, NULL, NULL, NULL);
		if (rst == -1) {
			CFC_LOG_ERROR("worker select failure");
			continue;
		} else if (rst == 0) {
			continue;
		}

		if (FD_ISSET(notify, &read_set)) {
			char tmp;
			int result;
			cfc_item_t *item;
			for (;;) {
				result = read(notify, &tmp, 1);
				if ((result == -1 || tmp != '\0')
					&& result != 0) {
					break;
				}

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
					result = redis_incr(item->buffer);
					pefree(item, 1);
				}

				if (result == 0) {
					pthread_exit(0);
				}
			}
		}
	}

	return NULL;
}

void *cfc_thread_queue(void *arg)
{
	fd_set read_set;
	int queue = manager_ptr->queues[0];
	char read_buf[BUFFER_SIZE];
	for (;;) {

		FD_ZERO(&read_set);
		FD_SET(queue, &read_set);

		int rst = select(queue + 1, &read_set, NULL, NULL, NULL);
		if (rst == -1) {
			CFC_LOG_ERROR("queue select failure");
			continue;
		} else if (rst == 0) {
			continue;
		}

#define check_read_result(r) \
		if (r == -1) { \
			break; \
		} \
		if (r == 0) { \
			pthread_exit(0); \
		}

		if (FD_ISSET(queue, &read_set)) {
			char *offset;
			size_t len;
			int r;
			for (;;) {
				memset(read_buf, 0, BUFFER_SIZE);
				r = read(queue, &len, sizeof(size_t));
				check_read_result(r);
				r = read(queue, read_buf, len);
				check_read_result(r);
				if (r != len) {
					CFC_LOG_WARN("read failure");
					break;
				}
				cfc_item_t *item;
				item = (cfc_item_t *)pemalloc(sizeof(*item) + len, 1);
				if (!item) {
					CFC_LOG_WARN("Memory malloc failure");
					continue;
				}

				item->size = len;
				item->next = NULL;
				memcpy(item->buffer, read_buf, len);

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
		}
	}
	return NULL;
}

int cfc_init(void)
{
#define CLOSE_PIPE \
		close(manager_ptr->notifiers[0]); \
		close(manager_ptr->notifiers[1]); \
		close(manager_ptr->queues[0]); \
		close(manager_ptr->queues[1])

	manager_ptr->head = NULL;
	manager_ptr->tail = NULL;
	manager_ptr->qlock = 0;

	if (pipe(manager_ptr->notifiers) == -1) {
		return -1;
	}

	if (pipe(manager_ptr->queues) == -1) {
		return -1;
	}

	if (cfc_init_log(cfc_logfile, CFC_LOG_LEVEL_DEBUG) == -1) {
		CLOSE_PIPE;
		return -1;
	}

	set_nonblocking(manager_ptr->notifiers[0]);
	set_nonblocking(manager_ptr->notifiers[1]);

	set_nonblocking(manager_ptr->queues[0]);
	set_nonblocking(manager_ptr->queues[1]);

	if (pthread_create(&worker_tid, NULL,
		cfc_thread_worker, NULL) == -1)
	{
		CFC_LOG_ERROR("Work thread start failure");
		CLOSE_PIPE;
		return -1;
	}

	if (pthread_create(&queue_tid, NULL,
		cfc_thread_queue, NULL) == -1)
	{
		CFC_LOG_ERROR("Queue thread start failure");
		CLOSE_PIPE;
		return -1;
	}


	if (redis_init() == -1) {
		CFC_LOG_ERROR("Redis initialize failure");
		return -1;
	}
	return 0;

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

ZEND_INI_MH(php_cfc_ht_name)
{
	if (!new_value || new_value->len == 0) {
		return FAILURE;
	}

	cfc_ht_name = strdup(new_value->val);
	if (cfc_ht_name == NULL) {
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
	PHP_INI_ENTRY("cfc.ht_name", "cfc_hash", PHP_INI_ALL, php_cfc_ht_name)
PHP_INI_END()

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(cfc)
{
	REGISTER_INI_ENTRIES();
	if (!cfc_enable) {
		return SUCCESS;
	}
	spin_init();
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
	UNREGISTER_INI_ENTRIES();
	if (!cfc_enable) {
		return SUCCESS;
	}
	// 通知线程退出
	close(manager_ptr->queues[1]);
	if (queue_tid) {
		pthread_join(queue_tid, NULL);
	}

	close(manager_ptr->notifiers[1]);
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
