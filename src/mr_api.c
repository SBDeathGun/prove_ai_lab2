/*
 * mr_api.c - Interfaccia pubblica del framework libmr e orchestrazione
 * della pipeline.
 *
 * Contiene l'implementazione delle funzioni esposte in mr.h
 * (mr_attr_*, mr_create, mr_start, mr_destroy) e la logica che, nel
 * processo principale, crea i processi figli e trasporta i dati.
 *
 * La pipeline e' composta da tre processi e tre pipe anonime:
 *
 *   Processo principale --(main_to_mapper)--> Mapper
 *   Mapper              --(mapper_to_reducer)--> Reducer
 *   Reducer             --(reducer_to_main)--> Processo principale
 *
 * La funzione mr_start() e' bloccante: crea le pipe, esegue i due fork(),
 * collega stdin e stdout dei figli con dup2(), chiude in ogni processo i
 * descrittori non utilizzati (condizione essenziale per la propagazione
 * dell'EOF), invia le righe, raccoglie i risultati, scrive il file di
 * output e attende i figli con waitpid().
 *
 * Non viene usato exec(): i figli ereditano il codice del processo
 * chiamante e con esso i puntatori alle funzioni mapper e reducer.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "mr.h"
#include "mr_common.h"
#include "mr_mapper.h"
#include "mr_reducer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

/*
 * Definizione della struttura opaca mr_t: il programma utente non ne
 * conosce i campi, ma vede solo il puntatore mr_t.
 *
 *   attr    : copia degli attributi di configurazione effettuata da
 *             mr_create(); il chiamante puo' quindi distruggere o
 *             modificare il proprio mr_attr_t senza effetti;
 *   mapper  : puntatore alla funzione mapper dell'utente, ereditato dai
 *             processi figli tramite fork();
 *   reducer : puntatore alla funzione reducer dell'utente;
 *   user_arg: argomento utente, inoltrato invariato alle due callback.
 *
 * Non esistono variabili globali o statiche condivise: piu' istanze mr_t
 * sono quindi utilizzabili contemporaneamente nello stesso processo senza
 * interferenze (requisito dell'addendum).
 */
struct mr {
    mr_attr_t attr;
    mr_mapper_t mapper;
    mr_reducer_t reducer;
    void *user_arg;
};

/*
 * Risultato finale in memoria, in attesa di essere ordinato e scritto nel
 * file di output. arrival_order conserva l'ordine di arrivo sulla pipe,
 * usato come criterio di paragone secondario in caso di piu' risultati per
 * lo stesso token, cosi' da rendere l'output completamente deterministico
 * (Sezione 8).
 */
typedef struct {
    char *token;
    size_t token_len;
    void *result;
    size_t result_len;
    size_t arrival_order;
} final_result_item_t;

/*
 * mr_attr_init - Inizializzazione degli attributi con valori di default.
 *
 * I default scelti sono 2 thread mapper, 2 thread reducer, code da 64
 * elementi, file di log predefinito (mr.log) e funzione di hashing di
 * default. I numeri di thread e la dimensione delle code sono sempre >= 1,
 * come richiesto dal contratto pubblico.
 *
 * attr : puntatore alla struttura da inizializzare.
 *
 * Ritorna 0 in caso di successo, -1 se attr e' NULL.
 */
int mr_attr_init(mr_attr_t *attr) {
    if (attr == NULL) {
        return -1;
    }
    attr->mapper_threads = 2;
    attr->reducer_threads = 2;
    attr->queue_size = 64;
    attr->log_file = NULL;
    attr->hash = mr_default_hash;
    attr->hash_arg = NULL;
    return 0;
}

/*
 * mr_attr_destroy - Distruzione degli attributi.
 *
 * Riporta tutti i campi al loro stato neutro. Non effettua deallocazioni,
 * perche' gli attributi non possiedono risorse proprie: log_file e hash_arg
 * sono puntatori di proprieta' del chiamante e vengono semplicemente
 * azzerati.
 *
 * attr : struttura da distruggere.
 *
 * Ritorna 0 in caso di successo, -1 se attr e' NULL.
 */
int mr_attr_destroy(mr_attr_t *attr) {
    if (attr == NULL) {
        return -1;
    }
    attr->mapper_threads = 0;
    attr->reducer_threads = 0;
    attr->queue_size = 0;
    attr->log_file = NULL;
    attr->hash = NULL;
    attr->hash_arg = NULL;
    return 0;
}

/*
 * mr_attr_set_mapper_threads - Impostazione del numero di thread mapper.
 *
 * attr : attributi da modificare.
 * n    : numero di thread; 0 e' rifiutato perche' il processo mapper deve
 *        essere multithread.
 *
 * Ritorna 0 in caso di successo, -1 se attr e' NULL o n vale 0.
 */
int mr_attr_set_mapper_threads(mr_attr_t *attr, size_t n) {
    if (attr == NULL || n == 0) {
        return -1;
    }
    attr->mapper_threads = n;
    return 0;
}

/*
 * mr_attr_set_reducer_threads - Impostazione del numero di thread reducer.
 *
 * attr : attributi da modificare.
 * n    : numero di thread; 0 e' rifiutato perche' il processo reducer deve
 *        essere multithread.
 *
 * Ritorna 0 in caso di successo, -1 se attr e' NULL o n vale 0.
 */
int mr_attr_set_reducer_threads(mr_attr_t *attr, size_t n) {
    if (attr == NULL || n == 0) {
        return -1;
    }
    attr->reducer_threads = n;
    return 0;
}

/*
 * mr_attr_set_queue_size - Impostazione della capacita' delle code interne.
 *
 * Il valore riguarda soltanto le code usate dal framework per coordinare i
 * thread C11 nei processi mapper e reducer: non ha alcun effetto sulla
 * dimensione delle pipe del sistema operativo. Uno zero non e' valido,
 * perche' una coda di capacita' nulla bloccherebbe i produttori.
 *
 * attr : attributi da modificare.
 * n    : capacita' massima in numero di elementi.
 *
 * Ritorna 0 in caso di successo, -1 se attr e' NULL o n vale 0.
 */
int mr_attr_set_queue_size(mr_attr_t *attr, size_t n) {
    if (attr == NULL || n == 0) {
        return -1;
    }
    attr->queue_size = n;
    return 0;
}

/*
 * mr_attr_set_log_file - Impostazione del percorso del file di log.
 *
 * Il puntatore viene memorizzato, non copiato: deve quindi restare valido
 * per tutta la durata dell'elaborazione. NULL (o una stringa vuota)
 * significa usare il nome predefinito "mr.log".
 *
 * attr : attributi da modificare.
 * path : percorso del file di log.
 *
 * Ritorna 0 in caso di successo, -1 se attr e' NULL.
 */
int mr_attr_set_log_file(mr_attr_t *attr, const char *path) {
    if (attr == NULL) {
        return -1;
    }
    attr->log_file = path;
    return 0;
}

/*
 * mr_attr_set_hash_function - Impostazione della funzione di hashing dei
 * token (addendum).
 *
 * La funzione indicata serve a partizionare in modo deterministico i
 * token fra i thread del processo reducer: due occorrenze dello stesso
 * token devono essere assegnate allo stesso worker. Passing NULL come
 * hash ripristina il comportamento predefinito (mr_default_hash).
 *
 * attr     : attributi da modificare.
 * hash     : funzione di hashing, oppure NULL per il default.
 * hash_arg : argomento opzionale passato alla funzione di hashing.
 *
 * Ritorna 0 in caso di successo, -1 se attr e' NULL.
 */
int mr_attr_set_hash_function(mr_attr_t *attr, mr_hash_t hash, void *hash_arg) {
    if (attr == NULL) {
        return -1;
    }
    attr->hash = hash;
    attr->hash_arg = hash_arg;
    return 0;
}

/*
 * mr_create - Creazione di una nuova istanza di elaborazione.
 *
 * Alloca la struttura opaca e vi copia gli attributi ricevuti: dopo il
 * ritorno con successo il chiamante puo' distruggere o modificare il
 * proprio mr_attr_t senza alterare l'elaborazione. Non viene eseguito
 * alcun fork() ne' creato alcun thread: l'avvio avviene in mr_start().
 *
 * mr       : indirizzo in cui depositare l'handle creato.
 * attr     : attributi di configurazione; NULL per i valori di default.
 * mapper   : funzione mapper dell'utente (obbligatoria).
 * reducer  : funzione reducer dell'utente (obbligatoria).
 * user_arg : argomento opzionale inoltrato alle callback.
 *
 * Ritorna 0 in caso di successo, -1 se mancano i puntatori obbligatori,
 * se gli attributi sono incoerenti o se l'allocazione fallisce.
 */
int mr_create(
    mr_t *mr,
    const mr_attr_t *attr,
    mr_mapper_t mapper,
    mr_reducer_t reducer,
    void *user_arg
) {
    if (mr == NULL || mapper == NULL || reducer == NULL) {
        return -1;
    }

    struct mr *inst = (struct mr *)calloc(1, sizeof(struct mr));
    if (inst == NULL) {
        return -1;
    }

    if (attr != NULL) {
        if (attr->mapper_threads == 0 || attr->reducer_threads == 0 || attr->queue_size == 0) {
            free(inst);
            return -1;
        }
        inst->attr = *attr;
    } else {
        mr_attr_init(&inst->attr);
    }

    if (inst->attr.hash == NULL) {
        inst->attr.hash = mr_default_hash;
    }

    inst->mapper = mapper;
    inst->reducer = reducer;
    inst->user_arg = user_arg;

    *mr = inst;
    return 0;
}

/*
 * mr_destroy - Distruzione dell'istanza e rilascio della memoria.
 *
 * Deve essere chiamata solo a elaborazione conclusa: mr_start() e' infatti
 * bloccante, quindi non vi sono strutture in uso al momento della
 * chiamata. La funzione tollera NULL e ritorna sempre 0.
 *
 * mr : handle restituito da mr_create().
 */
int mr_destroy(mr_t mr) {
    if (mr == NULL) {
        return 0;
    }
    free(mr);
    return 0;
}

/*
 * compare_strings - Comparatore di stringhe per qsort.
 *
 * Utilizzato per ordinare lessicograficamente i nomi dei file di input,
 * cosi' da rendere deterministico l'ordine di lettura richiesto dalla
 * specifica (indipendentemente dall'ordine restituito da readdir()).
 *
 * Ritorna un valore negativo, nullo o positivo a seconda dell'ordine.
 */
static int compare_strings(const void *a, const void *b) {
    const char *str1 = *(const char **)a;
    const char *str2 = *(const char **)b;
    return strcmp(str1, str2);
}

/*
 * collect_regular_files - Raccolta dei file regolari contenuti in una
 * directory, con supporto alla scansione ricorsiva (addendum).
 *
 * Apre la directory, ignora "." e ".." ed esamina ogni voce con stat():
 *   - se e' una sottodirectory, la funzione viene richiamata ricorsivamente
 *     (la scansione delle sottodirectory e' richiesta dall'addendum);
 *   - se e' un file regolare, il percorso viene duplicato e accodato
 *     nell'array di uscita, che viene riallocato con raddoppio della
 *     capacita' quando risulta pieno.
 *
 * L'ordinamento lessicografico non viene applicato qui, ma dal chiamante
 * dopo la raccolta completa.
 *
 * dir_path  : percorso della directory da esaminare.
 * files_out : indirizzo dell'array dei percorsi (accumulo delle righe).
 * count_out : numero di percorsi attualmente raccolti.
 * cap_out   : capacita' corrente dell'array.
 *
 * Ritorna 0 in caso di successo, -1 in caso di errore di apertura della
 * directory o di allocazione.
 */
static int collect_regular_files(const char *dir_path, char ***files_out, size_t *count_out, size_t *cap_out) {
    DIR *d = opendir(dir_path);
    if (d == NULL) {
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char full_path[4096];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) {
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            /* Supporto addendum per scansione ricorsiva delle directory */
            if (collect_regular_files(full_path, files_out, count_out, cap_out) != 0) {
                closedir(d);
                return -1;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (*count_out == *cap_out) {
                size_t new_cap = (*cap_out == 0) ? 16 : (*cap_out * 2);
                char **new_arr = (char **)realloc(*files_out, sizeof(char *) * new_cap);
                if (new_arr == NULL) {
                    closedir(d);
                    return -1;
                }
                *files_out = new_arr;
                *cap_out = new_cap;
            }
            (*files_out)[*count_out] = strdup(full_path);
            (*count_out)++;
        }
    }

    closedir(d);
    return 0;
}

/*
 * compare_results - Comparatore dei risultati finali per qsort.
 *
 * L'ordinamento e' lessicografico per token; a parita' di token (cioe'
 * quando il reducer ha emesso piu' risultati per la stessa chiave) si
 * usa l'ordine di arrivo sulla pipe come secondo criterio. Il risultato
 * e' che, a parita' di input e di callback, il file di output e'
 * identico fra esecuzioni diverse, pur con elaborazione concorrente e
 * scheduling non deterministico dei thread (Sezione 8).
 *
 * Ritorna un valore negativo, nullo o positivo a seconda dell'ordine.
 */
static int compare_results(const void *a, const void *b) {
    const final_result_item_t *item1 = (const final_result_item_t *)a;
    const final_result_item_t *item2 = (const final_result_item_t *)b;

    int cmp = strcmp(item1->token, item2->token);
    if (cmp != 0) {
        return cmp;
    }

    /* A parità di token, rispetta l'ordine deterministico di emissione */
    if (item1->arrival_order < item2->arrival_order) {
        return -1;
    }
    if (item1->arrival_order > item2->arrival_order) {
        return 1;
    }
    return 0;
}

/*
 * mr_start - Avvio e completamento di un'elaborazione MapReduce.
 *
 * Funzione bloccante: restituisce il controllo solo quando la pipeline e'
 * terminata correttamente oppure quando si e' verificato un errore.
 * Le fasi sono numerate nei commenti interni e seguono l'ordine prescritto
 * dalla Sezione 5 della specifica:
 *
 *    1. ispezione dell'input e raccolta dei file da elaborare;
 *    2. creazione delle tre pipe;
 *    3. fork() del processo mapper e collegamento delle pipe con dup2();
 *    4. fork() del processo reducer e collegamento delle pipe con dup2();
 *    5. invio delle righe serializzate al mapper e chiusura della pipe;
 *    6. raccolta dei risultati serializzati provenienti dal reducer;
 *    7. ordinamento deterministico dei risultati per token;
 *    8. scrittura del file di output in formato a record con lunghezze;
 *    9. attesa della terminazione dei figli con waitpid().
 *
 * In ogni processo vengono chiusi i descrittori non utilizzati: e' la
 * condizione che permette la corretta propagazione dell'EOF lungo tutta
 * la pipeline e che evita blocchi permanenti.
 *
 * mr          : handle dell'istanza.
 * input_path  : file regolare o directory di input.
 * output_path : percorso del file di output da produrre.
 *
 * Ritorna 0 in caso di successo, -1 in caso di errore (errno impostato
 * dove appropriato).
 */
int mr_start(mr_t mr, const char *input_path, const char *output_path) {
    if (mr == NULL || input_path == NULL || output_path == NULL) {
        return -1;
    }

    struct timespec start_time, end_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    mr_logger_t logger;
    if (mr_logger_open(&logger, mr->attr.log_file) != 0) {
        return -1;
    }

    mr_log_msg(&logger, "main", "main", "INIZIO_ELABORAZIONE", "Avvio elaborazione MapReduce (input: %s, output: %s)",
               input_path, output_path);

    /* 1. Ispezione input: file singolo o directory */
    struct stat st;
    if (stat(input_path, &st) != 0) {
        mr_log_msg(&logger, "main", "main", "ERRORE", "Input path non valido: %s", strerror(errno));
        mr_logger_close(&logger);
        return -1;
    }

    char **input_files = NULL;
    size_t files_count = 0;
    size_t files_capacity = 0;

    if (S_ISDIR(st.st_mode)) {
        if (collect_regular_files(input_path, &input_files, &files_count, &files_capacity) != 0) {
            mr_log_msg(&logger, "main", "main", "ERRORE", "Errore nella lettura della directory di input");
            mr_logger_close(&logger);
            return -1;
        }
        /* REQUISITO: Ordinamento lessicografico dei file di input per determinismo */
        if (files_count > 1) {
            qsort(input_files, files_count, sizeof(char *), compare_strings);
        }
    } else if (S_ISREG(st.st_mode)) {
        input_files = (char **)malloc(sizeof(char *));
        input_files[0] = strdup(input_path);
        files_count = 1;
    } else {
        mr_log_msg(&logger, "main", "main", "ERRORE", "Input non e' un file regolare ne' una directory");
        mr_logger_close(&logger);
        return -1;
    }

    mr_log_msg(&logger, "main", "main", "APERTURA_INPUT", "Trovati %zu file regolari da elaborare", files_count);

    /* 2. Creazione delle tre pipe della pipeline */
    int main_to_mapper[2];
    int mapper_to_reducer[2];
    int reducer_to_main[2];

    if (pipe(main_to_mapper) != 0 || pipe(mapper_to_reducer) != 0 || pipe(reducer_to_main) != 0) {
        mr_log_msg(&logger, "main", "main", "ERRORE", "Fallimento creazione pipe: %s", strerror(errno));
        for (size_t i = 0; i < files_count; i++) free(input_files[i]);
        free(input_files);
        mr_logger_close(&logger);
        return -1;
    }

    mr_log_msg(&logger, "main", "main", "CREAZIONE_PIPE", "Create pipe di comunicazione della pipeline");

    /* 3. Creazione del processo Mapper tramite fork() */
    pid_t mapper_pid = fork();
    if (mapper_pid < 0) {
        mr_log_msg(&logger, "main", "main", "ERRORE", "Fork mapper fallita: %s", strerror(errno));
        /* Pulizia e uscita */
        for (size_t i = 0; i < files_count; i++) free(input_files[i]);
        free(input_files);
        mr_logger_close(&logger);
        return -1;
    }

    if (mapper_pid == 0) {
        /* PROCESSO FIGLIO: MAPPER */
        dup2(main_to_mapper[0], STDIN_FILENO);
        dup2(mapper_to_reducer[1], STDOUT_FILENO);

        /* Chiusura di tutti i descrittori non necessari */
        close(main_to_mapper[0]);
        close(main_to_mapper[1]);
        close(mapper_to_reducer[0]);
        close(mapper_to_reducer[1]);
        close(reducer_to_main[0]);
        close(reducer_to_main[1]);

        mapper_process_main(mr->attr.queue_size, mr->attr.mapper_threads, mr->mapper, mr->user_arg, &logger);
        _exit(0);
    }

    mr_log_msg(&logger, "main", "main", "CREAZIONE_PROCESSO_MAPPER", "Creato processo mapper (PID: %d)", mapper_pid);

    /* 4. Creazione del processo Reducer tramite fork() */
    pid_t reducer_pid = fork();
    if (reducer_pid < 0) {
        mr_log_msg(&logger, "main", "main", "ERRORE", "Fork reducer fallita: %s", strerror(errno));
        /* Pulizia */
        for (size_t i = 0; i < files_count; i++) free(input_files[i]);
        free(input_files);
        mr_logger_close(&logger);
        return -1;
    }

    if (reducer_pid == 0) {
        /* PROCESSO FIGLIO: REDUCER */
        dup2(mapper_to_reducer[0], STDIN_FILENO);
        dup2(reducer_to_main[1], STDOUT_FILENO);

        /* Chiusura di tutti i descrittori non necessari */
        close(main_to_mapper[0]);
        close(main_to_mapper[1]);
        close(mapper_to_reducer[0]);
        close(mapper_to_reducer[1]);
        close(reducer_to_main[0]);
        close(reducer_to_main[1]);

        reducer_process_main(mr->attr.queue_size, mr->attr.reducer_threads, mr->reducer, mr->user_arg,
                             mr->attr.hash, mr->attr.hash_arg, &logger);
        _exit(0);
    }

    mr_log_msg(&logger, "main", "main", "CREAZIONE_PROCESSO_REDUCER", "Creato processo reducer (PID: %d)", reducer_pid);

    /* Nel processo Main: chiusura dei descrittori non necessari per consentire propagazione EOF */
    close(main_to_mapper[0]);
    close(mapper_to_reducer[0]);
    close(mapper_to_reducer[1]);
    close(reducer_to_main[1]);

    /* 5. Invio delle righe al Mapper */
    size_t total_lines_sent = 0;

    for (size_t f = 0; f < files_count; f++) {
        const char *filename = input_files[f];
        FILE *fp = fopen(filename, "rb");
        if (fp == NULL) {
            mr_log_msg(&logger, "main", "main", "ERRORE", "Impossibile aprire file input %s: %s", filename, strerror(errno));
            continue;
        }

        char *line_buf = NULL;
        size_t line_cap = 0;
        ssize_t nread;
        unsigned long line_num = 1;

        while ((nread = getline(&line_buf, &line_cap, fp)) != -1) {
            size_t len = (size_t)nread;
            /* Rimuove eventuale newline terminale '\n' */
            if (len > 0 && line_buf[len - 1] == '\n') {
                len--;
                /* Gestione eventuale carriage return '\r' su file DOS */
                if (len > 0 && line_buf[len - 1] == '\r') {
                    len--;
                }
            }

            mr_line_msg_hdr_t hdr;
            hdr.file_name_len = (int)strlen(filename);
            hdr.line_len = (int)len;
            hdr.line_number = line_num++;

            /* Invio header, filename e riga */
            mr_writen(main_to_mapper[1], &hdr, sizeof(hdr));
            if (hdr.file_name_len > 0) {
                mr_writen(main_to_mapper[1], filename, (size_t)hdr.file_name_len);
            }
            if (hdr.line_len > 0) {
                mr_writen(main_to_mapper[1], line_buf, (size_t)hdr.line_len);
            }

            total_lines_sent++;
        }

        free(line_buf);
        fclose(fp);
    }

    for (size_t i = 0; i < files_count; i++) {
        free(input_files[i]);
    }
    free(input_files);

    mr_log_msg(&logger, "main", "main", "STATISTICHE_RIGHE", "Completato invio righe al Mapper: inviate %zu righe in totale", total_lines_sent);

    /*
     * Chiusura del lato di scrittura verso il mapper:
     * Genera la ricezione di EOF sul Mapper.
     */
    close(main_to_mapper[1]);

    /* 6. Ricezione dei risultati finali dal Reducer */
    size_t results_cap = 64;
    size_t results_count = 0;
    final_result_item_t *results = (final_result_item_t *)malloc(sizeof(final_result_item_t) * results_cap);

    while (1) {
        mr_result_header_t hdr;
        ssize_t ret = mr_readn(reducer_to_main[0], &hdr, sizeof(hdr));
        if (ret == 0) {
            /* EOF dal Reducer: tutti i risultati sono stati inviati */
            break;
        }
        if (ret < 0) {
            mr_log_msg(&logger, "main", "main", "ERRORE", "Errore ricezione risultati da reducer: %s", strerror(errno));
            break;
        }

        if (hdr.token_len <= 0 || hdr.token_len > MR_MAX_TOKEN_LEN ||
            hdr.result_len < 0 || hdr.result_len > MR_MAX_RESULT_LEN) {
            mr_log_msg(&logger, "main", "main", "ERRORE", "Header risultato non valido (token_len=%d, result_len=%d)",
                       hdr.token_len, hdr.result_len);
            break;
        }

        char *token = (char *)malloc((size_t)hdr.token_len + 1);
        mr_readn(reducer_to_main[0], token, (size_t)hdr.token_len);
        token[hdr.token_len] = '\0';

        void *res_bytes = NULL;
        if (hdr.result_len > 0) {
            res_bytes = malloc((size_t)hdr.result_len);
            mr_readn(reducer_to_main[0], res_bytes, (size_t)hdr.result_len);
        }

        if (results_count == results_cap) {
            results_cap *= 2;
            final_result_item_t *new_res = (final_result_item_t *)realloc(results, sizeof(final_result_item_t) * results_cap);
            if (new_res == NULL) {
                free(token);
                free(res_bytes);
                break;
            }
            results = new_res;
        }

        results[results_count].token = token;
        results[results_count].token_len = (size_t)hdr.token_len;
        results[results_count].result = res_bytes;
        results[results_count].result_len = (size_t)hdr.result_len;
        results[results_count].arrival_order = results_count;
        results_count++;
    }

    close(reducer_to_main[0]);

    /* 7. Ordinamento deterministico dei risultati per token */
    if (results_count > 1) {
        qsort(results, results_count, sizeof(final_result_item_t), compare_results);
    }

    /* 8. Scrittura del file di output in formato binario a record */
    int out_fd = open(output_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        mr_log_msg(&logger, "main", "main", "ERRORE", "Impossibile creare file di output %s: %s", output_path, strerror(errno));
        /* Pulizia */
        for (size_t i = 0; i < results_count; i++) {
            free(results[i].token);
            free(results[i].result);
        }
        free(results);
        mr_logger_close(&logger);
        return -1;
    }

    mr_log_msg(&logger, "main", "main", "APERTURA_OUTPUT", "Scrittura di %zu risultati nel file di output %s",
               results_count, output_path);

    for (size_t i = 0; i < results_count; i++) {
        mr_output_record_hdr_t rec_hdr;
        rec_hdr.token_len = (int)results[i].token_len;
        rec_hdr.result_len = (int)results[i].result_len;

        mr_writen(out_fd, &rec_hdr, sizeof(rec_hdr));
        if (rec_hdr.token_len > 0) {
            mr_writen(out_fd, results[i].token, (size_t)rec_hdr.token_len);
        }
        if (rec_hdr.result_len > 0 && results[i].result != NULL) {
            mr_writen(out_fd, results[i].result, (size_t)rec_hdr.result_len);
        }

        free(results[i].token);
        free(results[i].result);
    }
    free(results);

    close(out_fd);
    mr_log_msg(&logger, "main", "main", "CHIUSURA_OUTPUT", "File di output chiuso con successo");

    /* 9. Attesa terminazione processi figli */
    int status_m = 0, status_r = 0;
    waitpid(mapper_pid, &status_m, 0);
    waitpid(reducer_pid, &status_r, 0);

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    double elapsed_sec = (double)(end_time.tv_sec - start_time.tv_sec) +
                         (double)(end_time.tv_nsec - start_time.tv_nsec) / 1e9;

    mr_log_msg(&logger, "main", "main", "TERMINE_ELABORAZIONE",
               "Elaborazione completata in %.4f secondi. Righe: %zu, Risultati: %zu",
               elapsed_sec, total_lines_sent, results_count);

    /* Generazione file statistiche (Addendum) */
    char stats_filename[1024];
    snprintf(stats_filename, sizeof(stats_filename), "%s.stats", output_path);
    FILE *stats_fp = fopen(stats_filename, "w");
    if (stats_fp != NULL) {
        fprintf(stats_fp, "=== STATISTICHE ESECUZIONE MAPREDUCE ===\n");
        fprintf(stats_fp, "Tempo di esecuzione: %.4f secondi\n", elapsed_sec);
        fprintf(stats_fp, "Numero di righe lette: %zu\n", total_lines_sent);
        fprintf(stats_fp, "Numero di risultati emessi: %zu\n", results_count);
        fclose(stats_fp);
    }

    mr_logger_close(&logger);
    return 0;
}
