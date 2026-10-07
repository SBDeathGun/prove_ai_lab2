/*
 * mr_common.c - Primitive comuni del framework libmr.
 *
 * Contiene i mattoni condivisi da tutti gli altri moduli:
 *   - le primitive di I/O robusto sulle pipe (mr_readn, mr_writen), che
 *     gestiscono letture/scritture parziali (Sezione 7 della specifica);
 *   - la validazione dei token alfanumerici ASCII (Sezione 3);
 *   - la funzione di hashing di default per il partizionamento dei token;
 *   - il logger sincronizzato fra thread (mtx_t) e fra processi (flock),
 *     con il formato [timestamp] [processo] [thread] [evento] messaggio
 *     richiesto dalla Sezione 11.
 *
 * Nessuna di queste funzioni interpreta il contenuto dei valori intermedi
 * o dei risultati: questi dati sono trattati come sequenze opache di byte.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "mr_common.h"

#include <stdarg.h>
#include <time.h>
#include <ctype.h>

/*
 * mr_readn - Lettura completa di n byte da un file descriptor.
 *
 * Non e' corretto assumere che una singola read() trasferisca un intero
 * messaggio logico: il protocollo interno richiede quindi di ripetere la
 * chiamata finche' tutti i byte richiesti non sono stati letti. Gli
 * errori interrupting (EINTR) sono ritentati, in modo che un segnale non
 * interrompa la ricezione di un messaggio a meta'.
 *
 * fd      : file descriptor da cui leggere (tipicamente una pipe).
 * buf     : buffer di destinazione, deve avere spazio per n byte.
 * n       : numero di byte da leggere.
 *
 * Ritorna:
 *   - n, se sono stati letti tutti i byte richiesti;
 *   -  0, se si e' incontrato EOF pulito prima di leggere il primo byte;
 *   - -1, in caso di errore oppure di EOF prematuro (il messaggio
 *         corrente e' troncato e non puo' essere considerato valido).
 */
ssize_t mr_readn(int fd, void *buf, size_t n) {
    size_t nleft = n;
    char *ptr = (char *)buf;

    while (nleft > 0) {
        ssize_t nread = read(fd, ptr, nleft);
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        } else if (nread == 0) {
            /* EOF riscontrato */
            if (nleft == n) {
                return 0; /* EOF pulito prima di iniziare il messaggio */
            } else {
                /* EOF prematuro */
                return -1;
            }
        }
        nleft -= (size_t)nread;
        ptr += nread;
    }
    return (ssize_t)n;
}

/*
 * mr_writen - Scrittura completa di n byte su un file descriptor.
 *
 * Simmetrica a mr_readn: la pipe ha una capacita' finita e una singola
 * write() puo' scrivere solo una parte del buffer. I byte sono quindi
 * scritti in cicli successivi finche' il buffer e' esaurito, ritentando
 * anche in caso di EINTR. La scrittura parziale e' pero' responsibility
 * del chiamante: chi invoca questa funzione deve garantire che il
 * messaggio logico non venga mescolato con altri (nel mapper e nel
 * reducer questo e' assicurato tenendo il mutex di uscita per tutta la
 * durata della scrittura).
 *
 * fd  : file descriptor su cui scrivere (tipicamente una pipe).
 * buf : buffer sorgente, non modificato.
 * n   : numero di byte da scrivere.
 *
 * Ritorna n se tutti i byte sono stati scritti, -1 in caso di errore.
 */
ssize_t mr_writen(int fd, const void *buf, size_t n) {
    size_t nleft = n;
    const char *ptr = (const char *)buf;

    while (nleft > 0) {
        ssize_t nwritten = write(fd, ptr, nleft);
        if (nwritten < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        nleft -= (size_t)nwritten;
        ptr += nwritten;
    }
    return (ssize_t)n;
}

/*
 * mr_is_valid_token - Verifica della forma dei token prodotti dal mapper.
 *
 * Un token e' una sequenza non vuota di caratteri alfanumerici ASCII
 * (A-Z, a-z, 0-9). Il token e' l'unico campo interpretato dal framework
 * e viene usato come chiave di raggruppamento, mentre il valore
 * intermedio resta opaco: e' quindi lecito confrontarlo con strcmp e
 *ordinalo lessicograficamente, ma non con funzioni applicate ai valori.
 *
 * La funzione lavora su (token, len) e non richiede che il buffer sia
 * terminato da '\0'.
 *
 * Ritorna true se il token e' conforme al contratto, false altrimenti.
 */
bool mr_is_valid_token(const char *token, size_t len) {
    if (token == NULL || len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = token[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            return false;
        }
    }
    return true;
}

/*
 * mr_default_hash - Funzione di hashing deterministica predefinita (DJB2).
 *
 * Usata dall'addendum quando l'utente non fornisce una funzione di
 * hashing propria. Il requisito e' che sia deterministica (due
 * occorrenze dello stesso token devono produrre lo stesso valore, cosi'
 * che tutte le coppie dello stesso token finiscano dallo stesso worker),
 * che non modifichi il token e che non dipenda dall'ordine di
 * esecuzione dei thread: per questo il calcolo usa soltanto i byte del
 * token e il proprio stato locale.
 *
 * token     : token da hashare (stringa C terminata da '\0').
 * token_len : lunghezza del token in byte.
 * user_arg  : ignorato dalla funzione di default.
 *
 * Ritorna il valore di hash associato al token.
 */
size_t mr_default_hash(const char *token, size_t token_len, void *user_arg) {
    (void)user_arg;
    size_t hash = 5381;
    for (size_t i = 0; i < token_len; i++) {
        hash = ((hash << 5) + hash) + (unsigned char)token[i];
    }
    return hash;
}

/*
 * mr_logger_open - Apertura del file di log e inizializzazione del mutex.
 *
 * Se path e' NULL o vuoto viene usato il nome predefinito "mr.log".
 * Il file e' aperto in append, cosi' che le righe prodotte dai processi
 * figli ereditati tramite fork() si accodino a quelle gia' scritte dal
 * processo principale senza essere sovrascritte. In ogni caso, l'accesso
 * al file e' serializzato (mtx_t fra i thread dello stesso processo,
 * flock fra processi diversi), quindi l'ordine delle righe non
 * garantito e' irrilevante ai fini dell'analisi del log.
 *
 * logger : struttura logger da inizializzare.
 * path   : percorso del file di log, oppure NULL per il default.
 *
 * Ritorna 0 in caso di successo, -1 in caso di errore (errno impostato
 * da fopen).
 */
int mr_logger_open(mr_logger_t *logger, const char *path) {
    if (logger == NULL) {
        return -1;
    }
    const char *actual_path = (path != NULL && path[0] != '\0') ? path : "mr.log";
    strncpy(logger->path, actual_path, sizeof(logger->path) - 1);
    logger->path[sizeof(logger->path) - 1] = '\0';

    logger->fp = fopen(logger->path, "a");
    if (logger->fp == NULL) {
        return -1;
    }

    if (mtx_init(&logger->local_mtx, mtx_plain) != thrd_success) {
        fclose(logger->fp);
        logger->fp = NULL;
        return -1;
    }

    return 0;
}

/*
 * mr_logger_close - Chiusura del file di log e rilascio del mutex.
 *
 * La chiusura del FILE* e' effettuata tenendo il mutex, cosi' che
 * nessun thread in procinto di scrivere possa trovare il file gia'
 * chiuso. La funzione e' idempotente e tollera un logger non valido:
 * puo' essere chiamata su un logger gia' chiuso o mai aperto.
 *
 * logger : struttura logger da chiudere.
 */
void mr_logger_close(mr_logger_t *logger) {
    if (logger == NULL || logger->fp == NULL) {
        return;
    }
    mtx_lock(&logger->local_mtx);
    fclose(logger->fp);
    logger->fp = NULL;
    mtx_unlock(&logger->local_mtx);
    mtx_destroy(&logger->local_mtx);
}

/*
 * mr_log_msg - Scrittura di una riga di log nel formato della specifica.
 *
 * Formato della riga: [timestamp] [processo] [thread] [evento] messaggio
 *
 * Il messaggio viene formattato con il formato printf dell'utente e
 * scritto come singola riga. La sincronizzazione avviene su due livelli:
 *   - mtx_t (logger->local_mtx) serializza i thread dello stesso
 *     processo;
 *   - flock(fd, LOCK_EX) serializza i processi diversi, che condividono
 *     lo stesso file di log attraverso l'ereditata da fork().
 *
 * logger   : logger gia' aperto; se NULL o non aperto la chiamata e' un
 *            no-op, cosi' da non dover controllare il logger ad ogni sito.
 * proc_name: nome del processo emittente ("main", "mapper", "reducer").
 * thrd_name: nome del thread emittente; NULL indica il thread principale.
 * event    : etichetta dell'evento ("AVVIO_THREAD", "ERRORE", ...);
 *            NULL viene sostituito da "INFO".
 * fmt      : formato del messaggio, come in printf.
 */
void mr_log_msg(mr_logger_t *logger, const char *proc_name, const char *thrd_name, const char *event, const char *fmt, ...) {
    if (logger == NULL || logger->fp == NULL) {
        return;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_buf;
    localtime_r(&ts.tv_sec, &tm_buf);

    char time_str[64];
    snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d.%03ld",
             tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
             tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
             ts.tv_nsec / 1000000L);

    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    /* Sincronizzazione locale (tra thread) e inter-processo (tra processi) */
    mtx_lock(&logger->local_mtx);
    int fd = fileno(logger->fp);
    if (fd >= 0) {
        flock(fd, LOCK_EX);
    }

    fprintf(logger->fp, "[%s] [%s] [%s] [%s] %s\n",
            time_str,
            proc_name ? proc_name : "main",
            thrd_name ? thrd_name : "main",
            event ? event : "INFO",
            buffer);
    fflush(logger->fp);

    if (fd >= 0) {
        flock(fd, LOCK_UN);
    }
    mtx_unlock(&logger->local_mtx);
}
