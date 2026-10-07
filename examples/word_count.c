#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mr.h"

/*
 * Callback mapper per Word Count:
 * Estrae token alfanumerici dalla riga ed emette (token, 1).
 */
static int word_count_mapper(
    const mr_file_line_t *line,
    mr_emit_pair_t emit,
    void *emit_arg,
    void *user_arg
) {
    (void)user_arg;
    if (line == NULL || line->line == NULL || line->line_len == 0) {
        return 0;
    }

    const char *text = line->line;
    size_t len = line->line_len;
    size_t i = 0;

    int one = 1;

    while (i < len) {
        /* Salta caratteri non alfanumerici */
        while (i < len && !isalnum((unsigned char)text[i])) {
            i++;
        }
        if (i >= len) {
            break;
        }

        size_t start = i;
        while (i < len && isalnum((unsigned char)text[i])) {
            i++;
        }
        size_t token_len = i - start;

        char token_buf[256];
        if (token_len < sizeof(token_buf)) {
            for (size_t k = 0; k < token_len; k++) {
                token_buf[k] = text[start + k];
            }
            token_buf[token_len] = '\0';

            /* Emissione della coppia <token, 1> */
            if (emit(token_buf, &one, sizeof(one), emit_arg) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

/*
 * Callback reducer per Word Count:
 * Somma tutti i valori interi associati al token ed emette la somma totale.
 */
static int word_count_reducer(
    const char *token,
    const mr_value_t *values,
    size_t values_count,
    mr_emit_result_t emit,
    void *emit_arg,
    void *user_arg
) {
    (void)user_arg;
    int total = 0;

    for (size_t i = 0; i < values_count; i++) {
        if (values[i].size == sizeof(int) && values[i].data != NULL) {
            int val = *(const int *)values[i].data;
            total += val;
        }
    }

    /* Emissione del risultato finale */
    return emit(token, &total, sizeof(total), emit_arg);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Uso: %s <input_file_o_dir> <output_file.mro> [log_file]\n", argv[0]);
        return 1;
    }

    const char *input_path = argv[1];
    const char *output_path = argv[2];
    const char *log_path = (argc >= 4) ? argv[3] : "mr_word_count.log";

    mr_attr_t attr;
    if (mr_attr_init(&attr) != 0) {
        perror("mr_attr_init");
        return 1;
    }

    if (mr_attr_set_mapper_threads(&attr, 4) != 0 ||
        mr_attr_set_reducer_threads(&attr, 4) != 0 ||
        mr_attr_set_queue_size(&attr, 64) != 0 ||
        mr_attr_set_log_file(&attr, log_path) != 0) {
        perror("mr_attr_set_*");
        mr_attr_destroy(&attr);
        return 1;
    }

    mr_t mr;
    if (mr_create(&mr, &attr, word_count_mapper, word_count_reducer, NULL) != 0) {
        perror("mr_create");
        mr_attr_destroy(&attr);
        return 1;
    }

    printf("Avvio conteggio parole con MapReduce...\n");
    printf("Input: %s\nOutput: %s\nLog: %s\n", input_path, output_path, log_path);

    if (mr_start(mr, input_path, output_path) != 0) {
        perror("mr_start");
        mr_destroy(mr);
        mr_attr_destroy(&attr);
        return 1;
    }

    printf("Elaborazione completata con successo!\n");

    mr_destroy(mr);
    mr_attr_destroy(&attr);
    return 0;
}
