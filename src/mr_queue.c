#include "mr_queue.h"

#include <stdlib.h>

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
