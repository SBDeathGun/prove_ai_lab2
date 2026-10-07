#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "mr_reducer.h"
#include "mr_queue.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <threads.h>

#define HASH_TABLE_BUCKETS 8192

typedef struct {
    void *data;
    size_t size;
} stored_value_t;

typedef struct token_group {
    char *token;
    size_t token_len;
    stored_value_t *values;
    size_t values_count;
    size_t values_capacity;
    struct token_group *next;
} token_group_t;

typedef struct {
    token_group_t *buckets[HASH_TABLE_BUCKETS];
    size_t distinct_tokens;
} group_table_t;

typedef struct {
    int worker_id;
    mr_queue_t *queue;
    mr_reducer_t reducer_cb;
    void *user_arg;
    int out_fd;
    mtx_t *out_mtx;
    size_t *results_count;
    mtx_t *stats_mtx;
    mr_logger_t *logger;
} reducer_worker_arg_t;

typedef struct {
    const char *expected_token;
    int out_fd;
    mtx_t *out_mtx;
    size_t *results_count;
    mtx_t *stats_mtx;
    mr_logger_t *logger;
} reducer_emit_ctx_t;

static size_t internal_hash(const char *str, size_t len) {
    size_t h = 5381;
    for (size_t i = 0; i < len; i++) {
        h = ((h << 5) + h) + (unsigned char)str[i];
    }
    return h % HASH_TABLE_BUCKETS;
}

static token_group_t *table_find_or_create(group_table_t *table, const char *token, size_t token_len) {
    size_t b = internal_hash(token, token_len);
    token_group_t *curr = table->buckets[b];

    while (curr != NULL) {
        if (curr->token_len == token_len && strcmp(curr->token, token) == 0) {
            return curr;
        }
        curr = curr->next;
    }

    /* Crea nuovo gruppo */
    token_group_t *new_grp = (token_group_t *)calloc(1, sizeof(token_group_t));
    if (new_grp == NULL) {
        return NULL;
    }

    new_grp->token = strdup(token);
    if (new_grp->token == NULL) {
        free(new_grp);
        return NULL;
    }

    new_grp->token_len = token_len;
    new_grp->values_capacity = 4;
    new_grp->values = (stored_value_t *)malloc(sizeof(stored_value_t) * new_grp->values_capacity);
    if (new_grp->values == NULL) {
        free(new_grp->token);
        free(new_grp);
        return NULL;
    }

    new_grp->next = table->buckets[b];
    table->buckets[b] = new_grp;
    table->distinct_tokens++;

    return new_grp;
}

static int table_add_value(token_group_t *grp, const void *val, size_t val_size) {
    if (grp->values_count == grp->values_capacity) {
        size_t new_cap = grp->values_capacity * 2;
        stored_value_t *new_vals = (stored_value_t *)realloc(grp->values, sizeof(stored_value_t) * new_cap);
        if (new_vals == NULL) {
            return -1;
        }
        grp->values = new_vals;
        grp->values_capacity = new_cap;
    }

    stored_value_t *slot = &grp->values[grp->values_count];
    slot->size = val_size;
    if (val_size > 0 && val != NULL) {
        slot->data = malloc(val_size);
        if (slot->data == NULL) {
            return -1;
        }
        char *dst = (char *)slot->data;
        const char *src = (const char *)val;
        for (size_t i = 0; i < val_size; i++) {
            dst[i] = src[i];
        }
    } else {
        slot->data = NULL;
    }

    grp->values_count++;
    return 0;
}

static int reducer_emit_callback(const char *token, const void *result, size_t result_size, void *emit_arg) {
    if (token == NULL || emit_arg == NULL) {
        return -1;
    }

    size_t token_len = strlen(token);
    if (!mr_is_valid_token(token, token_len)) {
        return -1;
    }

    if (token_len > MR_MAX_TOKEN_LEN || result_size > MR_MAX_RESULT_LEN) {
        return -1;
    }

    if (result_size > 0 && result == NULL) {
        return -1;
    }

    reducer_emit_ctx_t *ctx = (reducer_emit_ctx_t *)emit_arg;

    /* Nel contratto base, il token emesso deve coincidere con il token del gruppo */
    if (ctx->expected_token != NULL && strcmp(token, ctx->expected_token) != 0) {
        return -1;
    }

    mr_result_header_t hdr;
    hdr.token_len = (int)token_len;
    hdr.result_len = (int)result_size;

    /* Scrittura atomica del record risultato su STDOUT verso il Main */
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

    if (result_size > 0 && result != NULL) {
        if (mr_writen(ctx->out_fd, result, result_size) < 0) {
            mtx_unlock(ctx->out_mtx);
            return -1;
        }
    }

    mtx_lock(ctx->stats_mtx);
    (*(ctx->results_count))++;
    mtx_unlock(ctx->stats_mtx);

    mtx_unlock(ctx->out_mtx);
    return 0;
}

static int reducer_worker_main(void *arg) {
    reducer_worker_arg_t *w_arg = (reducer_worker_arg_t *)arg;
    char thrd_name[32];
    snprintf(thrd_name, sizeof(thrd_name), "worker-%d", w_arg->worker_id);

    mr_log_msg(w_arg->logger, "reducer", thrd_name, "AVVIO_THREAD", "Avviato worker reducer");

    void *item = NULL;
    while (mr_queue_pop(w_arg->queue, &item) == 1) {
        token_group_t *grp = (token_group_t *)item;

        /* Costruzione array mr_value_t per la callback */
        mr_value_t *values_array = NULL;
        if (grp->values_count > 0) {
            values_array = (mr_value_t *)malloc(sizeof(mr_value_t) * grp->values_count);
            if (values_array != NULL) {
                for (size_t i = 0; i < grp->values_count; i++) {
                    values_array[i].data = grp->values[i].data;
                    values_array[i].size = grp->values[i].size;
                }
            }
        }

        reducer_emit_ctx_t emit_ctx = {
            .expected_token = grp->token,
            .out_fd = w_arg->out_fd,
            .out_mtx = w_arg->out_mtx,
            .results_count = w_arg->results_count,
            .stats_mtx = w_arg->stats_mtx,
            .logger = w_arg->logger
        };

        /* Invocazione callback reducer una sola volta per il token completo */
        w_arg->reducer_cb(grp->token, values_array, grp->values_count, reducer_emit_callback, &emit_ctx, w_arg->user_arg);

        free(values_array);

        /* Liberazione risorse del gruppo */
        for (size_t i = 0; i < grp->values_count; i++) {
            free(grp->values[i].data);
        }
        free(grp->values);
        free(grp->token);
        free(grp);
    }

    mr_log_msg(w_arg->logger, "reducer", thrd_name, "TERMINE_THREAD", "Terminato worker reducer");
    return 0;
}

int reducer_process_main(
    size_t queue_size,
    size_t reducer_threads,
    mr_reducer_t reducer_cb,
    void *user_arg,
    mr_hash_t hash_func,
    void *hash_arg,
    mr_logger_t *logger
) {
    mr_log_msg(logger, "reducer", "main", "AVVIO_PROCESSO", "Processo Reducer avviato con %zu worker thread (queue_size=%zu)",
               reducer_threads, queue_size);

    group_table_t table = {0};

    mr_hash_t actual_hash = (hash_func != NULL) ? hash_func : mr_default_hash;

    /*
     * FASE 1: Lettura delle coppie da STDIN (pipe dal Mapper) e raggruppamento per token.
     * Continua finché il Mapper non chiude la pipe (ricezione di EOF).
     */
    mr_log_msg(logger, "reducer", "reader", "AVVIO_THREAD", "Avviata lettura coppie e raggruppamento");

    size_t pairs_received = 0;
    while (1) {
        mr_pair_header_t hdr;
        ssize_t ret = mr_readn(STDIN_FILENO, &hdr, sizeof(hdr));
        if (ret == 0) {
            /* EOF: Il Mapper ha chiuso la pipe, tutte le coppie sono state inviate */
            break;
        }
        if (ret < 0) {
            mr_log_msg(logger, "reducer", "reader", "ERRORE", "Errore di lettura header coppia da stdin: %s", strerror(errno));
            break;
        }

        /* Validazione di sicurezza delle lunghezze */
        if (hdr.token_len <= 0 || hdr.token_len > MR_MAX_TOKEN_LEN ||
            hdr.value_len < 0 || hdr.value_len > MR_MAX_VALUE_LEN) {
            mr_log_msg(logger, "reducer", "reader", "ERRORE", "Header coppia non valido (token_len=%d, value_len=%d)",
                       hdr.token_len, hdr.value_len);
            break;
        }

        char *token_buf = (char *)malloc((size_t)hdr.token_len + 1);
        if (token_buf == NULL) {
            break;
        }

        if (mr_readn(STDIN_FILENO, token_buf, (size_t)hdr.token_len) <= 0) {
            free(token_buf);
            break;
        }
        token_buf[hdr.token_len] = '\0'; /* Terminatore nullo locale */

        void *val_buf = NULL;
        if (hdr.value_len > 0) {
            val_buf = malloc((size_t)hdr.value_len);
            if (val_buf == NULL) {
                free(token_buf);
                break;
            }
            if (mr_readn(STDIN_FILENO, val_buf, (size_t)hdr.value_len) <= 0) {
                free(val_buf);
                free(token_buf);
                break;
            }
        }

        token_group_t *grp = table_find_or_create(&table, token_buf, (size_t)hdr.token_len);
        if (grp != NULL) {
            table_add_value(grp, val_buf, (size_t)hdr.value_len);
        }

        free(val_buf);
        free(token_buf);
        pairs_received++;
    }

    mr_log_msg(logger, "reducer", "reader", "STATISTICHE_TOKEN", "Raggruppamento completato: ricevute %zu coppie, %zu token distinti",
               pairs_received, table.distinct_tokens);

    /*
     * FASE 2: Partizionamento e smistamento dei gruppi completi ai worker reducer C11.
     * Ciascun token viene instradato tramite funzione hash al rispettivo worker thread.
     */
    mr_queue_t **worker_queues = (mr_queue_t **)malloc(sizeof(mr_queue_t *) * reducer_threads);
    thrd_t *worker_thrds = (thrd_t *)malloc(sizeof(thrd_t) * reducer_threads);
    reducer_worker_arg_t *worker_args = (reducer_worker_arg_t *)malloc(sizeof(reducer_worker_arg_t) * reducer_threads);

    mtx_t out_mtx;
    mtx_t stats_mtx;
    mtx_init(&out_mtx, mtx_plain);
    mtx_init(&stats_mtx, mtx_plain);
    size_t results_count = 0;

    for (size_t i = 0; i < reducer_threads; i++) {
        worker_queues[i] = mr_queue_create(queue_size);
        worker_args[i].worker_id = (int)i;
        worker_args[i].queue = worker_queues[i];
        worker_args[i].reducer_cb = reducer_cb;
        worker_args[i].user_arg = user_arg;
        worker_args[i].out_fd = STDOUT_FILENO;
        worker_args[i].out_mtx = &out_mtx;
        worker_args[i].results_count = &results_count;
        worker_args[i].stats_mtx = &stats_mtx;
        worker_args[i].logger = logger;

        thrd_create(&worker_thrds[i], reducer_worker_main, &worker_args[i]);
    }

    /* Distribuzione dei gruppi ai worker tramite hashing deterministico */
    for (size_t b = 0; b < HASH_TABLE_BUCKETS; b++) {
        token_group_t *curr = table.buckets[b];
        while (curr != NULL) {
            token_group_t *next = curr->next;
            curr->next = NULL;

            size_t h = actual_hash(curr->token, curr->token_len, hash_arg);
            size_t target_worker = h % reducer_threads;
            mr_queue_push(worker_queues[target_worker], curr);

            curr = next;
        }
        table.buckets[b] = NULL;
    }

    /* Chiusura delle code dei worker */
    for (size_t i = 0; i < reducer_threads; i++) {
        mr_queue_close(worker_queues[i]);
    }

    /* Attesa della terminazione di tutti i worker reducer */
    for (size_t i = 0; i < reducer_threads; i++) {
        thrd_join(worker_thrds[i], NULL);
        mr_queue_destroy(worker_queues[i]);
    }

    mr_log_msg(logger, "reducer", "main", "STATISTICHE_RISULTATI", "Reducer completato: emessi %zu risultati finali", results_count);

    /*
     * REQUISITO TASSATIVO (Sezione 5.1):
     * Il reducer chiude il proprio STDOUT (pipe verso il main) solo dopo che
     * tutti i risultati sono stati scritti. Ciò permette al Main di ricevere EOF.
     */
    close(STDOUT_FILENO);

    free(worker_queues);
    free(worker_thrds);
    free(worker_args);
    mtx_destroy(&out_mtx);
    mtx_destroy(&stats_mtx);

    mr_log_msg(logger, "reducer", "main", "TERMINE_PROCESSO", "Processo Reducer terminato con successo");
    return 0;
}
