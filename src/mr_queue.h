#ifndef MR_QUEUE_H
#define MR_QUEUE_H

#include <stddef.h>
#include <stdbool.h>
#include <threads.h>

typedef struct mr_queue mr_queue_t;

/* Creazione e distruzione coda bounded */
mr_queue_t *mr_queue_create(size_t capacity);
void mr_queue_destroy(mr_queue_t *q);

/* Inserimento di un elemento (blocca se piena). Ritorna 0 se ok, -1 se chiusa o errore. */
int mr_queue_push(mr_queue_t *q, void *item);

/* Estrazione di un elemento (blocca se vuota). Ritorna 1 se estratto, 0 se vuota e chiusa, -1 se errore. */
int mr_queue_pop(mr_queue_t *q, void **item);

/* Chiusura della coda: non accetta più push, risveglia tutti i thread in attesa */
void mr_queue_close(mr_queue_t *q);

#endif /* MR_QUEUE_H */
