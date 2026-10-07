#ifndef MR_H
#define MR_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Handle opaco di una elaborazione. */
typedef struct mr *mr_t;

/*
 * Funzione di hashing opzionale per partizionare i token fra
 * i thread reducer interni al processo reducer.
 *
 * La funzione deve essere deterministica: due occorrenze dello stesso
 * token devono produrre lo stesso valore di hash.
 */
typedef size_t (*mr_hash_t)(
    const char *token,
    size_t token_len,
    void *user_arg
);

/*
 * Attributi di configurazione del framework.
 *
 * mapper_threads:
 *   numero di thread usati nel processo mapper (almeno 1).
 *
 * reducer_threads:
 *   numero di thread usati nel processo reducer (almeno 1).
 *
 * queue_size:
 *   capacita' massima delle code interne usate per coordinare
 *   i thread C11 nei processi mapper e reducer (almeno 1).
 *   Non rappresenta la dimensione delle pipe.
 *
 * log_file:
 *   nome del file di log, oppure NULL per usare il nome di default ("mr.log").
 *
 * hash:
 *   funzione di hashing opzionale (Addendum per partizionamento reducer).
 *
 * hash_arg:
 *   argomento opzionale passato alla funzione di hashing.
 */
typedef struct {
    size_t mapper_threads;
    size_t reducer_threads;
    size_t queue_size;
    const char *log_file;
    mr_hash_t hash;
    void *hash_arg;
} mr_attr_t;

/*
 * Riga logica di un file, vista dalla funzione mapper.
 *
 * I campi file_name e line sono puntatori validi nel processo mapper
 * solo durante l'invocazione della funzione mapper.
 *
 * file_name e line non devono necessariamente essere terminati da '\0'.
 * Le rispettive lunghezze sono indicate da file_name_len e line_len.
 */
typedef struct {
    const char *file_name;
    size_t file_name_len;
    unsigned long line_number;
    const char *line;
    size_t line_len;
} mr_file_line_t;

/*
 * Valore opaco associato a un token.
 *
 * Il framework non interpreta il contenuto di data.
 * Se size vale 0, data puo' essere NULL.
 */
typedef struct {
    const void *data;
    size_t size;
} mr_value_t;

/*
 * Funzione usata dal mapper per emettere una coppia <token, valore>.
 *
 * token deve essere una stringa C valida, terminata da '\0',
 * composta soltanto da caratteri alfanumerici ASCII.
 *
 * value e' una sequenza opaca di byte di lunghezza value_size.
 * Se value_size vale 0, value puo' essere NULL.
 */
typedef int (*mr_emit_pair_t)(
    const char *token,
    const void *value,
    size_t value_size,
    void *emit_arg
);

/*
 * Funzione usata dal reducer per emettere un risultato finale.
 *
 * token deve essere una stringa C valida. Nel progetto base, il token
 * emesso dal reducer deve coincidere con il token ricevuto dalla funzione reducer.
 *
 * result e' una sequenza opaca di byte di lunghezza result_size.
 * Se result_size vale 0, result puo' essere NULL.
 */
typedef int (*mr_emit_result_t)(
    const char *token,
    const void *result,
    size_t result_size,
    void *emit_arg
);

/*
 * Funzione mapper fornita dal programma utente.
 *
 * La funzione mapper riceve una riga logica e puo' emettere zero o piu'
 * coppie <token, valore> usando la funzione emit.
 */
typedef int (*mr_mapper_t)(
    const mr_file_line_t *line,
    mr_emit_pair_t emit,
    void *emit_arg,
    void *user_arg
);

/*
 * Funzione reducer fornita dal programma utente.
 *
 * La funzione reducer riceve un token e tutti i valori associati a quel
 * token. Puo' emettere zero o piu' risultati usando la funzione emit.
 */
typedef int (*mr_reducer_t)(
    const char *token,
    const mr_value_t *values,
    size_t values_count,
    mr_emit_result_t emit,
    void *emit_arg,
    void *user_arg
);

/* Inizializzazione e distruzione attributi */
int mr_attr_init(mr_attr_t *attr);
int mr_attr_destroy(mr_attr_t *attr);

/* Impostazione parametri di configurazione */
int mr_attr_set_mapper_threads(mr_attr_t *attr, size_t n);
int mr_attr_set_reducer_threads(mr_attr_t *attr, size_t n);
int mr_attr_set_queue_size(mr_attr_t *attr, size_t n);
int mr_attr_set_log_file(mr_attr_t *attr, const char *path);
int mr_attr_set_hash_function(mr_attr_t *attr, mr_hash_t hash, void *hash_arg);

/* Creazione, esecuzione e distruzione dell'istanza MapReduce */
int mr_create(
    mr_t *mr,
    const mr_attr_t *attr,
    mr_mapper_t mapper,
    mr_reducer_t reducer,
    void *user_arg
);

int mr_start(mr_t mr, const char *input_path, const char *output_path);
int mr_destroy(mr_t mr);

#ifdef __cplusplus
}
#endif

#endif /* MR_H */
