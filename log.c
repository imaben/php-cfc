#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include "log.h"

static char *cfc_error_titles[CFC_LOG_LEVEL_ERROR + 1] = {
    "DEBUG", "NOTICE", "WARN", "ERROR"
};

static int cfc_log_fd = 0;
static int cfc_log_mark = CFC_LOG_LEVEL_DEBUG;
static int cfc_log_initialize = 0;
static char cfc_log_buffer[4096];


int cfc_init_log(char *file, int mark)
{
    if (cfc_log_initialize) {
        return 0;
    }

    if (mark < CFC_LOG_LEVEL_DEBUG
        || mark > CFC_LOG_LEVEL_ERROR)
    {
        return -1;
    }

    if (file) {
        cfc_log_fd = open(file, O_WRONLY | O_CREAT | O_APPEND , 0666);
        if (!cfc_log_fd) {
            return -1;
        }

    } else {
        dup2(cfc_log_fd, STDERR_FILENO);
    }

    cfc_log_mark = mark;
    cfc_log_initialize = 1;

    return 0;
}


void cfc_log(int level, char *fmt, ...)
{
    va_list al;
    time_t current;
    struct tm *dt;
    int off1, off2;

    if (!cfc_log_initialize
        || level < cfc_log_mark
        || level > CFC_LOG_LEVEL_ERROR)
    {
        return;
    }

    /* Get current date and time */
    time(&current);
    dt = localtime(&current);

    off1 = sprintf(cfc_log_buffer,
                   "[%04d-%02d-%02d %02d:%02d:%02d] %s: ",
                   dt->tm_year + 1900,
                   dt->tm_mon + 1,
                   dt->tm_mday,
                   dt->tm_hour,
                   dt->tm_min,
                   dt->tm_sec,
                   cfc_error_titles[level]);

    va_start(al, fmt);
    off2 = vsprintf(cfc_log_buffer + off1, fmt, al);
    va_end(al);

    cfc_log_buffer[off1 + off2] = '\n';

    write(cfc_log_fd, cfc_log_buffer, off1 + off2 + 1);
}


void cfc_destroy_log()
{
    if (!cfc_log_initialize) {
        return;
    }

    if (cfc_log_fd && cfc_log_fd != STDERR_FILENO) {
        close(cfc_log_fd);
    }
}

int cfc_log_get_fd()
{
    return cfc_log_fd;
}
