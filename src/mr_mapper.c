#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "mr_mapper.h"
#include "mr_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <threads.h>

typedef struct {
    char *file_name;
    size_t file_name_len;
    unsigned long line_number;
    char *line;
    size_t line_len;
} mapper_task_t;

typedef struct {
    mr_queue_t *queue;
    mr_logger_t *logger;
    size_t lines_count;
} reader_arg_t;

typedef struct {
    int worker_id;
    mr_queue_t *queue;
    mr_mapper_t mapper_cb;
    void *user_arg;
    int out_fd;
    mtx_t *out_mtx;
    size_t *pairs_count;
    mtx_t *stats_mtx;
    mr_logger_t *logger;
} worker_arg_t;

typedef struct {
    int out_fd;
    mtx_t *out_mtx;
    size_t *pairs_count;
    mtx_t *stats_mtx;
    mr_logger_t *logger;
} emit_ctx_t;

static int mapper_emit_callback(const char *token, const void *value, size_t value_size, void *emit_arg) {
    if (token == NULL || emit_arg == NULL) {
        return -1;
    }

    size_t token_len = strlen(token);
    if (!mr_is_valid_token(token, token_len)) {
        return -1;
    }

    if (token_len > MR_MAX_TOKEN_LEN || value_size > MR_MAX_VALUE_LEN) {
        return -1;
    }

    if (value_size > 0 && value == NULL) {
        return -1;
    }

    emit_ctx_t *ctx = (emit_ctx_t *)emit_arg;

    mr_pair_header_t hdr;
    hdr.token_len = (int)token_len;
    hdr.value_len = (int)value_size;

    /* Scrittura atomica del messaggio intero (header + token + valore) sulla pipe */
    mtx_lock(ctx->out_mtx);

    if (mr_writen(ctx->out_fd, &hdr, sizeof(hdr)) < 0) {
        mtx_unlock(ctx->out_mtx);
        return -1;
    }

    if (token_len > 0) {
        if (mr_writen(ctx->out_fd, token, token_len) < 0) {
            mtx_unlock(ctx->out_mtx);
            return -1;
        }
    }

    if (value_size > 0 && value != NULL) {
        if (mr_writen(ctx->out_fd, value, value_size) < 0) {
            mtx_unlock(ctx->out_mtx);
            return -1;
        }
    }

    mtx_lock(ctx->stats_mtx);
    (*(ctx->pairs_count))++;
    mtx_unlock(ctx->stats_mtx);

    mtx_unlock(ctx->out_mtx);
    return 0;
}

static int reader_main(void *arg) {
    reader_arg_t *r_arg = (reader_arg_t *)arg;
    mr_log_msg(r_arg->logger, "mapper", "reader", "AVVIO_THREAD", "Avviato thread reader mapper");

    while (1) {
        mr_line_msg_hdr_t hdr;
        ssize_t ret = mr_readn(STDIN_FILENO, &hdr, sizeof(hdr));
        if (ret == 0) {
            /* EOF: Fine delle righe inviate dal Main */
            break;
        }
        if (ret < 0) {
            mr_log_msg(r_arg->logger, "mapper", "reader", "ERRORE", "Errore di lettura header riga da stdin: %s", strerror(errno));
            break;
        }

        /* Validazione di sicurezza delle lunghezze ricevute */
        if (hdr.file_name_len < 0 || hdr.file_name_len > MR_MAX_PATH_LEN ||
            hdr.line_len < 0 || hdr.line_len > MR_MAX_LINE_LEN) {
            mr_log_msg(r_arg->logger, "mapper", "reader", "ERRORE", "Header riga non valido (file_name_len=%d, line_len=%d)",
                       hdr.file_name_len, hdr.line_len);
            break;
        }

        mapper_task_t *task = (mapper_task_t *)calloc(1, sizeof(mapper_task_t));
        if (task == NULL) {
            break;
        }
        task->file_name_len = (size_t)hdr.file_name_len;
        task->line_len = (size_t)hdr.line_len;
        task->line_number = hdr.line_number;

        if (task->file_name_len > 0) {
            task->file_name = (char *)malloc(task->file_name_len);
            if (task->file_name == NULL) {
                free(task);
                break;
            }
            if (mr_readn(STDIN_FILENO, task->file_name, task->file_name_len) <= 0) {
                free(task->file_name);
                free(task);
                break;
            }
        }

        if (task->line_len > 0) {
            task->line = (char *)malloc(task->line_len);
            if (task->line == NULL) {
                free(task->file_name);
                free(task);
                break;
            }
            if (mr_readn(STDIN_FILENO, task->line, task->line_len) <= 0) {
                free(task->line);
                free(task->file_name);
                free(task);
                break;
            }
        }

        r_arg->lines_count++;
        if (mr_queue_push(r_arg->queue, task) != 0) {
            free(task->line);
            free(task->file_name);
            free(task);
            break;
        }
    }

    mr_queue_close(r_arg->queue);
    mr_log_msg(r_arg->logger, "mapper", "reader", "TERMINE_THREAD", "Terminato thread reader mapper (righe lette: %zu)", r_arg->lines_count);
    return 0;
}

static int mapper_worker_main(void *arg) {
    worker_arg_t *w_arg = (worker_arg_t *)arg;
    char thrd_name[32];
    snprintf(thrd_name, sizeof(thrd_name), "worker-%d", w_arg->worker_id);

    mr_log_msg(w_arg->logger, "mapper", thrd_name, "AVVIO_THREAD", "Avviato worker mapper");

    emit_ctx_t emit_ctx = {
        .out_fd = w_arg->out_fd,
        .out_mtx = w_arg->out_mtx,
        .pairs_count = w_arg->pairs_count,
        .stats_mtx = w_arg->stats_mtx,
        .logger = w_arg->logger
    };

    void *item = NULL;
    while (mr_queue_pop(w_arg->queue, &item) == 1) {
        mapper_task_t *task = (mapper_task_t *)item;

        mr_file_line_t line_view;
        line_view.file_name = task->file_name;
        line_view.file_name_len = task->file_name_len;
        line_view.line_number = task->line_number;
        line_view.line = task->line;
        line_view.line_len = task->line_len;

        /* Invocazione della callback utente mapper */
        w_arg->mapper_cb(&line_view, mapper_emit_callback, &emit_ctx, w_arg->user_arg);

        free(task->line);
        free(task->file_name);
        free(task);
    }

    mr_log_msg(w_arg->logger, "mapper", thrd_name, "TERMINE_THREAD", "Terminato worker mapper");
    return 0;
}

int mapper_process_main(
    size_t queue_size,
    size_t mapper_threads,
    mr_mapper_t mapper_cb,
    void *user_arg,
    mr_logger_t *logger
) {
    mr_log_msg(logger, "mapper", "main", "AVVIO_PROCESSO", "Processo Mapper avviato con %zu worker thread (queue_size=%zu)",
               mapper_threads, queue_size);

    mr_queue_t *queue = mr_queue_create(queue_size);
    if (queue == NULL) {
        mr_log_msg(logger, "mapper", "main", "ERRORE", "Impossibile creare la coda produttore-consumatore");
        return -1;
    }

    mtx_t out_mtx;
    mtx_t stats_mtx;
    if (mtx_init(&out_mtx, mtx_plain) != thrd_success ||
        mtx_init(&stats_mtx, mtx_plain) != thrd_success) {
        mr_queue_destroy(queue);
        return -1;
    }

    size_t pairs_count = 0;

    /* Avvio del thread reader C11 */
    reader_arg_t r_arg = {
        .queue = queue,
        .logger = logger,
        .lines_count = 0
    };

    thrd_t reader_thrd;
    if (thrd_create(&reader_thrd, reader_main, &r_arg) != thrd_success) {
        mr_log_msg(logger, "mapper", "main", "ERRORE", "Fallita creazione del thread reader");
        mr_queue_destroy(queue);
        mtx_destroy(&out_mtx);
        mtx_destroy(&stats_mtx);
        return -1;
    }

    /* Avvio dei worker thread C11 */
    thrd_t *worker_thrds = (thrd_t *)malloc(sizeof(thrd_t) * mapper_threads);
    worker_arg_t *worker_args = (worker_arg_t *)malloc(sizeof(worker_arg_t) * mapper_threads);
    if (worker_thrds == NULL || worker_args == NULL) {
        /* Gestione errore di allocazione */
        mr_queue_close(queue);
        thrd_join(reader_thrd, NULL);
        free(worker_thrds);
        free(worker_args);
        mr_queue_destroy(queue);
        mtx_destroy(&out_mtx);
        mtx_destroy(&stats_mtx);
        return -1;
    }

    for (size_t i = 0; i < mapper_threads; i++) {
        worker_args[i].worker_id = (int)i;
        worker_args[i].queue = queue;
        worker_args[i].mapper_cb = mapper_cb;
        worker_args[i].user_arg = user_arg;
        worker_args[i].out_fd = STDOUT_FILENO;
        worker_args[i].out_mtx = &out_mtx;
        worker_args[i].pairs_count = &pairs_count;
        worker_args[i].stats_mtx = &stats_mtx;
        worker_args[i].logger = logger;

        if (thrd_create(&worker_thrds[i], mapper_worker_main, &worker_args[i]) != thrd_success) {
            mr_log_msg(logger, "mapper", "main", "ERRORE", "Errore creazione worker mapper %zu", i);
            /* Chiusura della coda per sbloccare i già avviati */
            mr_queue_close(queue);
            for (size_t j = 0; j < i; j++) {
                thrd_join(worker_thrds[j], NULL);
            }
            thrd_join(reader_thrd, NULL);
            free(worker_thrds);
            free(worker_args);
            mr_queue_destroy(queue);
            mtx_destroy(&out_mtx);
            mtx_destroy(&stats_mtx);
            return -1;
        }
    }

    /* Attesa del thread reader */
    thrd_join(reader_thrd, NULL);

    /* Attesa di tutti i worker mapper */
    for (size_t i = 0; i < mapper_threads; i++) {
        thrd_join(worker_thrds[i], NULL);
    }

    mr_log_msg(logger, "mapper", "main", "STATISTICHE_COPPIE", "Mapper completato: inviate %zu coppie al Reducer", pairs_count);

    /*
     * REQUISITO TASSATIVO (Sezione 5.1):
     * La pipe verso il reducer viene chiusa SOLO DOPO la terminazione di TUTTI
     * i thread mapper. Tale chiusura costituisce il segnale di EOF per il Reducer.
     */
    close(STDOUT_FILENO);

    free(worker_thrds);
    free(worker_args);
    mr_queue_destroy(queue);
    mtx_destroy(&out_mtx);
    mtx_destroy(&stats_mtx);

    mr_log_msg(logger, "mapper", "main", "TERMINE_PROCESSO", "Processo Mapper terminato con successo");
    return 0;
}
