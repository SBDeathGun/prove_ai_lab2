/*
 * word_count.c - Esempio di uso del framework libmr: conteggio delle
 * occorrenze dei token.
 *
 * Il programma fornisce al framework una funzione mapper che scandisce
 * ogni riga, ne estrae i token alfanumerici ed emette per ciascuno la
 * coppia <token, 1> (l'intero 1 serializzato come byte nativi), e una
 * funzione reducer che riceve tutti i valori associati ad un token, li
 * interpreta come interi e ne emette la somma.
 *
 * Il framework non contiene logica di conteggio parole: tutto il lavoro
 * specifico dell'analisi e' nelle due callback. In sede di valutazione
 * possono essere fornite callback diverse, conformi alla stessa
 * interfaccia.
 *
 * Uso: word_count <input_file_o_dir> <output_file.mro> [log_file]
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "mr.h"

/*
 * word_count_mapper - Callback mapper dell'esempio.
 *
 * Riceve una riga logica del file e, per ogni sequenza alfanumerica
 * individuata, emette la coppia <token, 1>. La funzione puo' emettere
 * zero coppie (per esempio su righe vuote o prive di caratteri
 * alfanumerici).
 *
 * Il token viene estratto qui, nel mapper: il framework non suddivide
 * la riga in token e si limita a trasportare i byte e a raggruppare i
 * risultati per chiave.
 *
 * line      : riga logica (file_name, line_number, line, line_len). Il
 *             contenuto non e' necessariamente terminato da '\0': la
 *             lunghezza va letta dal campo line_len.
 * emit      : funzione con cui emettere una coppia.
 * emit_arg  : contesto gestito dal framework, da passare a emit.
 * user_arg  : argomento utente (non utilizzato in questo esempio).
 *
 * Ritorna 0 se la riga e' stata elaborata, -1 in caso di errore di
 * emissione.
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
 * word_count_reducer - Callback reducer dell'esempio.
 *
 * Riceve un token e TUTTI i valori ad esso associati (il framework
 * garantisce che la funzione venga invocata una sola volta per token
 * distinto, mai una volta per coppia) e ne emette la somma.
 *
 * I valori sono dati opachi: l'unica chiarezza possibile e' la
 * dimensione dichiarata, qui confrontata con sizeof(int) prima di
 * reinterpretare i byte come intero.
 *
 * token        : token del gruppo, stringa C.
 * values       : array dei valori opachi associati al token.
 * values_count : numero di valori nel gruppo.
 * emit         : funzione con cui emettere il risultato finale.
 * emit_arg     : contesto gestito dal framework.
 * user_arg     : argomento utente (non utilizzato in questo esempio).
 *
 * Ritorna il valore restituito da emit (0 in caso di successo).
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

/*
 * main - Punto di ingresso del programma applicativo.
 *
 * Mostra la sequenza di uso dell'interfaccia pubblica: inizializzazione
 * e configurazione degli attributi, creazione dell'istanza con le due
 * callback, avento dell'elaborazione e distruzione delle risorse.
 * mr_start() e' bloccante: quando restituisce 0 l'elaborazione e'
 * terminata e il file di output e' stato prodotto.
 *
 * argv[1] : file regolare o directory di input.
 * argv[2] : percorso del file di output.
 * argv[3] : percorso del file di log (facoltativo).
 *
 * Ritorna 0 in caso di successo, 1 in caso di errore.
 */
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
