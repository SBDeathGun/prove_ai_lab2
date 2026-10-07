#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "mr_common.h"

#include <stdarg.h>
#include <time.h>
#include <ctype.h>

ssize_t mr_readn(int fd, void *buf, size_t n) {
    size_t nleft = n;
    char *ptr = (char *)buf;

    while (nleft > 0) {
        ssize_t nread = read(fd, ptr, nleft);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        } else if (nread == 0) {
            /* EOF riscontrato */
            if (nleft == n) {
                return 0; /* EOF pulito prima di iniziare il messaggio */
            } else {
                /* EOF prematuro */
                return -1;
            }
        }
        nleft -= (size_t)nread;
        ptr += nread;
    }
    return (ssize_t)n;
}

ssize_t mr_writen(int fd, const void *buf, size_t n) {
    size_t nleft = n;
    const char *ptr = (const char *)buf;

    while (nleft > 0) {
        ssize_t nwritten = write(fd, ptr, nleft);
        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        nleft -= (size_t)nwritten;
        ptr += nwritten;
    }
    return (ssize_t)n;
}

bool mr_is_valid_token(const char *token, size_t len) {
    if (token == NULL || len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = token[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

size_t mr_default_hash(const char *token, size_t token_len, void *user_arg) {
    (void)user_arg;
    size_t hash = 5381;
    for (size_t i = 0; i < token_len; i++) {
        hash = ((hash << 5) + hash) + (unsigned char)token[i];
    }
    return hash;
}

int mr_logger_open(mr_logger_t *logger, const char *path) {
    if (logger == NULL) {
        return -1;
    }
    const char *actual_path = (path != NULL && path[0] != '\0') ? path : "mr.log";
    strncpy(logger->path, actual_path, sizeof(logger->path) - 1);
    logger->path[sizeof(logger->path) - 1] = '\0';

    logger->fp = fopen(logger->path, "a");
    if (logger->fp == NULL) {
        return -1;
    }

    if (mtx_init(&logger->local_mtx, mtx_plain) != thrd_success) {
        fclose(logger->fp);
        logger->fp = NULL;
        return -1;
    }

    return 0;
}

void mr_logger_close(mr_logger_t *logger) {
    if (logger == NULL || logger->fp == NULL) {
        return;
    }
    mtx_lock(&logger->local_mtx);
    fclose(logger->fp);
    logger->fp = NULL;
    mtx_unlock(&logger->local_mtx);
    mtx_destroy(&logger->local_mtx);
}

void mr_log_msg(mr_logger_t *logger, const char *proc_name, const char *thrd_name, const char *event, const char *fmt, ...) {
    if (logger == NULL || logger->fp == NULL) {
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);

    char time_str[64];
    snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             ts.tv_nsec / 1000000L);

    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    /* Sincronizzazione locale (tra thread) e inter-processo (tra processi) */
    mtx_lock(&logger->local_mtx);
    int fd = fileno(logger->fp);
    if (fd >= 0) {
        flock(fd, LOCK_EX);
    }

    fprintf(logger->fp, "[%s] [%s] [%s] [%s] %s\n",
            time_str,
            proc_name ? proc_name : "main",
            thrd_name ? thrd_name : "main",
            event ? event : "INFO",
            buffer);
    fflush(logger->fp);

    if (fd >= 0) {
        flock(fd, LOCK_UN);
    }
    mtx_unlock(&logger->local_mtx);
}
