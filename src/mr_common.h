#ifndef MR_COMMON_H
#define MR_COMMON_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <threads.h>

#include "mr.h"

/* Limiti massimi di sicurezza per validazione degli header */
#define MR_MAX_PATH_LEN   (64 * 1024)          /* 64 KB */
#define MR_MAX_LINE_LEN   (64 * 1024 * 1024)   /* 64 MB */
#define MR_MAX_TOKEN_LEN  (1024 * 1024)        /* 1 MB */
#define MR_MAX_VALUE_LEN  (64 * 1024 * 1024)   /* 64 MB */
#define MR_MAX_RESULT_LEN (64 * 1024 * 1024)   /* 64 MB */

/* Header messaggio riga (Main -> Mapper) */
typedef struct {
    int file_name_len;
    int line_len;
    unsigned long line_number;
} mr_line_msg_hdr_t;

/* Header coppia intermedia (Mapper -> Reducer) conforme a Sezione 7 */
typedef struct {
    int token_len;
    int value_len;
} mr_pair_header_t;

/* Header risultato (Reducer -> Main) */
typedef struct {
    int token_len;
    int result_len;
} mr_result_header_t;

/* Header record nel file di output deterministico */
typedef struct {
    int token_len;
    int result_len;
} mr_output_record_hdr_t;

/* Struttura per il logging sincronizzato multi-processo e multi-thread */
typedef struct {
    FILE *fp;
    char path[1024];
    mtx_t local_mtx;
} mr_logger_t;

/* Primitive di I/O robusto su file descriptor */
ssize_t mr_readn(int fd, void *buf, size_t n);
ssize_t mr_writen(int fd, const void *buf, size_t n);

/* Validazione caratteri del token alfanumerico ASCII */
bool mr_is_valid_token(const char *token, size_t len);

/* Funzione hash di default deterministica (DJB2) */
size_t mr_default_hash(const char *token, size_t token_len, void *user_arg);

/* Funzioni per il sistema di logging */
int mr_logger_open(mr_logger_t *logger, const char *path);
void mr_logger_close(mr_logger_t *logger);
void mr_log_msg(mr_logger_t *logger, const char *proc_name, const char *thrd_name, const char *event, const char *fmt, ...);

#endif /* MR_COMMON_H */
