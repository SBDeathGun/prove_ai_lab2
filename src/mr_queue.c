/*
 * mr_queue.c - Coda circolare bounded sincronizzata con thread C11.
 *
 * Struttura dati interna ai processi mapper e reducer: NON e' una pipe e
 * NON e' memoria condivisa fra processi, ma una normale struttura in
 * memoria di processo che coordina i thread C11 secondo lo schema
 * produttore-consumatore richiesto dalla Sezione 6 della specifica.
 *
 * La sincronizzazione usa esclusivamente primitive C11 (mtx_t, cnd_t),
 * mai pthread. Il flag "closed" implementa la semantica di chiusura
 * necessaria per propagare l'EOF: quando la coda viene chiusa, i
 * consumatori in attesa vengono risvegliati e ricevono l'indicazione di
 * "coda vuota e chiusa" invece di bloccarsi per sempre.
 */
#include "mr_queue.h"

#include <stdlib.h>

/*
 * Struttura interna della coda (forward declaration in mr_queue.h):
 *
 *   buffer   : array circolare che memorizza i puntatori agli elementi;
 *   capacity : numero massimo di elementi (queue_size dell'attributo);
 *   head/tail: indici di lettura e scrittura, modulari sulla capacita';
 *   count    : numero di elementi attualmente presenti;
 *   closed   : coda chiusa, nessun nuovo inserimento accettato;
 *   mtx      : mutex che protegge l'intera struttura;
 *   not_full : condizione per risvegliare i produttori quando la coda
 *              non e' piena;
 *   not_empty: condizione per risvegliare i consumatori quando la coda
 *              contiene almeno un elemento.
 *
 * Il mutex viene usato anche come mutex condizionale: cnd_wait lo
 * rilascia automaticamente durante l'attesa.
 */
struct mr_queue {
    void **buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    bool closed;
    mtx_t mtx;
    cnd_t not_full;
    cnd_t not_empty;
};

/*
 * mr_queue_create - Creazione di una coda vuota con capacita' fissata.
 *
 * La capacita' corrisponde al parametro queue_size dell'attributo
 * mr_attr_t: si tratta della dimensione massima delle code interne del
 * framework, non della dimensione delle pipe del sistema operativo.
 *
 * capacity : numero massimo di elementi accodabili; 0 non e' valido.
 *
 * Ritorna la coda creata, oppure NULL se la capacita' e' nulla o se
 * fallisce un'allocazione o l'inizializzazione di mutex/condizioni
 * (in quest'ultimo caso tutte le risorse parzialmente acquisite vengono
 * liberate prima di restituire NULL).
 */
mr_queue_t *mr_queue_create(size_t capacity) {
    if (capacity == 0) {
        return NULL;
    }

    mr_queue_t *q = (mr_queue_t *)calloc(1, sizeof(mr_queue_t));
    if (q == NULL) {
        return NULL;
    }

    q->buffer = (void **)malloc(sizeof(void *) * capacity);
    if (q->buffer == NULL) {
        free(q);
        return NULL;
    }

    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->closed = false;

    if (mtx_init(&q->mtx, mtx_plain) != thrd_success) {
        free(q->buffer);
        free(q);
        return NULL;
    }

    if (cnd_init(&q->not_full) != thrd_success) {
        mtx_destroy(&q->mtx);
        free(q->buffer);
        free(q);
        return NULL;
    }

    if (cnd_init(&q->not_empty) != thrd_success) {
        cnd_destroy(&q->not_full);
        mtx_destroy(&q->mtx);
        free(q->buffer);
        free(q);
        return NULL;
    }

    return q;
}

/*
 * mr_queue_destroy - Chiusura e liberazione della coda.
 *
 * La coda viene prima chiusa (per risvegliare eventuali thread in
 * attesa) e poi distrutta: la distruzione e' sicura solo quando nessun
 * thread la sta piu' usando, quindi il chiamante deve aver prima atteso
 * la terminazione di tutti i consumatori e produttori.
 *
 * q : coda da distruggere; NULL e' tollerato e non fa nulla.
 */
void mr_queue_destroy(mr_queue_t *q) {
    if (q == NULL) {
        return;
    }

    mtx_lock(&q->mtx);
    q->closed = true;
    cnd_broadcast(&q->not_empty);
    cnd_broadcast(&q->not_full);
    mtx_unlock(&q->mtx);

    cnd_destroy(&q->not_empty);
    cnd_destroy(&q->not_full);
    mtx_destroy(&q->mtx);

    free(q->buffer);
    free(q);
}

/*
 * mr_queue_push - Inserimento di un elemento in coda (ruolo produttore).
 *
 * Se la coda e' piena il produttore si blocca sulla condizione not_full
 * finche' un consumatore non libera uno slot. La chiusura della coda
 * sblocca e fa fallire i produttori ancora in attesa.
 *
 * q    : coda su cui inserire.
 * item : puntatore all'elemento da accodare. La coda non copia e non
 *        libera il contenuto: la proprieta' dell'oggetto resta del
 *        produttore, che di norma libera il task dopo la presa in carico
 *        da un worker.
 *
 * Ritorna 0 se l'elemento e' stato accodato, -1 se la coda e' NULL,
 * chiusa, o se si e' verificato un errore. In caso di fallimento
 * l'elemento non e' stato accodato e resta al chiamante la responsabilita'
 * di liberarlo.
 */
int mr_queue_push(mr_queue_t *q, void *item) {
    if (q == NULL) {
        return -1;
    }

    mtx_lock(&q->mtx);
    while (q->count == q->capacity && !q->closed) {
        cnd_wait(&q->not_full, &q->mtx);
    }

    if (q->closed) {
        mtx_unlock(&q->mtx);
        return -1;
    }

    q->buffer[q->tail] = item;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;

    cnd_signal(&q->not_empty);
    mtx_unlock(&q->mtx);
    return 0;
}

/*
 * mr_queue_pop - Estrazione di un elemento dalla coda (ruolo consumatore).
 *
 * Se la coda e' vuota il consumatore si blocca sulla condizione not_empty.
 * La condizione d'attesa e' valutata in un ciclo while (e non in un if)
 * perche' la sveglia possa essere spuria: la coda potrebbe essere stata
 * nel frattempo svuotata da un altro consumatore.
 *
 * Segnali di fine flusso: quando la coda e' stata chiusa e si e' svuotata,
 * la funzione restituisce 0 e *item viene posto a NULL. I worker usano
 * questo valore per terminare il proprio ciclo di lavoro senza trattare
 * come dati un valore NULL.
 *
 * q    : coda da cui prelevare.
 * item : puntatore in cui viene depositato l'elemento estratto.
 *
 * Ritorna:
 *    1 se un elemento e' stato estratto;
 *    0 se la coda e' vuota e chiusa (condizione di fine, *item = NULL);
 *   -1 in caso di argomenti non validi.
 */
int mr_queue_pop(mr_queue_t *q, void **item) {
    if (q == NULL || item == NULL) {
        return -1;
    }

    mtx_lock(&q->mtx);
    while (q->count == 0 && !q->closed) {
        cnd_wait(&q->not_empty, &q->mtx);
    }

    if (q->count == 0 && q->closed) {
        mtx_unlock(&q->mtx);
        *item = NULL;
        return 0; /* EOF: Coda vuota e chiusa */
    }

    *item = q->buffer[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;

    cnd_signal(&q->not_full);
    mtx_unlock(&q->mtx);
    return 1;
}

/*
 * mr_queue_close - Chiusura della coda (segnalazione di fine dati).
 *
 * Non distrugge la coda: marca solo il campo closed e risveglia con un
 * broadcast tutti i thread in attesa su not_empty e not_full. I
 * consumatori potranno cosi' svuotare gli elementi residui prima di
 * ricevere l'indicazione di coda chiusa, mentre i produttori verranno
 * respinti. La funzione e' idempotente.
 *
 * q : coda da chiudere; NULL e' tollerato.
 */
void mr_queue_close(mr_queue_t *q) {
    if (q == NULL) {
        return;
    }

    mtx_lock(&q->mtx);
    q->closed = true;
    cnd_broadcast(&q->not_empty);
    cnd_broadcast(&q->not_full);
    mtx_unlock(&q->mtx);
}
