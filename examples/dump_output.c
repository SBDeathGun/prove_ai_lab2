/*
 * dump_output.c - Utility ausiliaria per ispezionare in forma leggibile il
 * file di output prodotto dal framework.
 *
 * Il file di output e' binario, a record con lunghezze esplicite: ogni
 * record e' composto da un header (token_len, result_len), dai byte del
 * token e dai byte del risultato. Questo programma lo decodifica e lo
 * stampa a terminale; non fa parte della libreria e serve solo a
 * verificare a mano il contenuto dell'output.
 *
 * Uso: dump_output <file_output.mro>
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include "mr.h"

/* Header di un record del file di output, replicato dal formato prodotto dal framework. */
typedef struct {
    int token_len;
    int result_len;
} output_record_hdr_t;

/*
 * safe_readn - Lettura completa di n byte, con gestione delle letture
 * parziali (un record puo' essere maggiore della dimensione del buffer
 * interno della pipe o del file).
 *
 * fd  : file descriptor da cui leggere.
 * buf : buffer di destinazione.
 * n   : numero di byte da leggere.
 *
 * Ritorna n se sono stati letti tutti i byte, 0 per EOF pulito prima di
 * iniziare e -1 per errore o EOF prematuro.
 */
static ssize_t safe_readn(int fd, void *buf, size_t n) {
    size_t nleft = n;
    char *ptr = (char *)buf;
    while (nleft > 0) {
        ssize_t nread = read(fd, ptr, nleft);
        if (nread < 0) {
            return -1;
        } else if (nread == 0) {
            if (nleft == n) return 0;
            return -1;
        }
        nleft -= (size_t)nread;
        ptr += nread;
    }
    return (ssize_t)n;
}

/*
 * main - Lettura sequenziale dei record del file di output e stampa.
 *
 * Per ogni record viene stampato il token e, quando il risultato e'
 * lungo esattamente sizeof(int), il suo valore come intero; in caso
 * contrario viene indicata soltanto la dimensione, dato che il risultato
 * e' un dato opaco che non e' detto essere una stringa.
 *
 * argv[1] : percorso del file di output da ispezionare.
 *
 * Ritorna 0 in caso di successo, 1 in caso di errore.
 */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Uso: %s <file_output.mro>\n", argv[0]);
        return 1;
    }

    const char *path = argv[1];
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    printf("=== DUMP DEL FILE DI OUTPUT %s ===\n", path);

    size_t count = 0;
    while (1) {
        output_record_hdr_t hdr;
        ssize_t ret = safe_readn(fd, &hdr, sizeof(hdr));
        if (ret == 0) {
            break; /* EOF */
        }
        if (ret < 0) {
            perror("safe_readn header");
            close(fd);
            return 1;
        }

        if (hdr.token_len < 0 || hdr.result_len < 0) {
            fprintf(stderr, "Record corrotto nel file\n");
            close(fd);
            return 1;
        }

        char *token = (char *)malloc((size_t)hdr.token_len + 1);
        if (safe_readn(fd, token, (size_t)hdr.token_len) <= 0) {
            free(token);
            break;
        }
        token[hdr.token_len] = '\0';

        void *result = NULL;
        if (hdr.result_len > 0) {
            result = malloc((size_t)hdr.result_len);
            if (safe_readn(fd, result, (size_t)hdr.result_len) <= 0) {
                free(token);
                free(result);
                break;
            }
        }

        if (hdr.result_len == sizeof(int) && result != NULL) {
            int val = *(int *)result;
            printf("[%zu] Token: '%s' -> Valore (int): %d\n", count, token, val);
        } else {
            printf("[%zu] Token: '%s' -> Valore (%d byte binari)\n", count, token, hdr.result_len);
        }

        free(token);
        free(result);
        count++;
    }

    printf("Totale record letti: %zu\n", count);
    close(fd);
    return 0;
}
