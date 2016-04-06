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

#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include "spin.h"

static int __cpus = 1;

void spin_init()
{
	__cpus = sysconf(_SC_NPROCESSORS_ONLN);
	if (__cpus <= 0) {
		__cpus = 1;
	}
}

void spin_lock(spin_t *lock)
{
	int i, n;

	for ( ;; ) {
		if (*lock == 0 &&
				__sync_bool_compare_and_swap(lock, 0, 1)) {
			return;
		}

		if (__cpus > 1) {
			for (n = 1; n < 129; n << 1) {
				for (i = 0; i < n; i++) {
					__asm__("pause");
				}

				if (*lock == 0 &&
						__sync_bool_compare_and_swap(lock, 0, 1)) {
					return;
				}
			}
		}

		sched_yield();
	}
}

void spin_unlock(spin_t *lock)
{
	__sync_bool_compare_and_swap(lock, 1, 0);
}

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
