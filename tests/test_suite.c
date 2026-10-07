#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

#include "mr.h"
#include "mr_common.h"

#define TEST_ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "\n[FALLITO] %s: %s (linea %d)\n", __func__, msg, __LINE__); \
        exit(1); \
    } \
} while (0)

/* Helper per creare file di testo */
static void create_file(const char *path, const char *content, size_t len) {
    FILE *fp = fopen(path, "wb");
    assert(fp != NULL);
    if (len > 0 && content != NULL) {
        fwrite(content, 1, len, fp);
    }
    fclose(fp);
}

/* Helper per confrontare due file bit-a-bit */
static int files_are_identical(const char *path1, const char *path2) {
    FILE *f1 = fopen(path1, "rb");
    FILE *f2 = fopen(path2, "rb");
    if (!f1 || !f2) {
        if (f1) fclose(f1);
        if (f2) fclose(f2);
        return 0;
    }

    int res = 1;
    while (1) {
        char buf1[4096], buf2[4096];
        size_t n1 = fread(buf1, 1, sizeof(buf1), f1);
        size_t n2 = fread(buf2, 1, sizeof(buf2), f2);
        if (n1 != n2) {
            res = 0;
            break;
        }
        if (n1 == 0) {
            break;
        }
        int diff = 0;
        for (size_t i = 0; i < n1; i++) {
            if (buf1[i] != buf2[i]) {
                diff = 1;
                break;
            }
        }
        if (diff) {
            res = 0;
            break;
        }
    }
    fclose(f1);
    fclose(f2);
    return res;
}

/* Mapper di base: parola -> int(1) */
static int test_mapper_simple(const mr_file_line_t *line, mr_emit_pair_t emit, void *emit_arg, void *user_arg) {
    (void)user_arg;
    if (line->line_len == 0) return 0;

    char token[64];
    size_t cur = 0;
    for (size_t i = 0; i < line->line_len; i++) {
        char c = line->line[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            if (cur < sizeof(token) - 1) {
                token[cur++] = c;
            }
        } else {
            if (cur > 0) {
                token[cur] = '\0';
                int one = 1;
                emit(token, &one, sizeof(one), emit_arg);
                cur = 0;
            }
        }
    }
    if (cur > 0) {
        token[cur] = '\0';
        int one = 1;
        emit(token, &one, sizeof(one), emit_arg);
    }
    return 0;
}

/* Reducer di base: somma degli interi */
static int test_reducer_sum(const char *token, const mr_value_t *values, size_t values_count,
                            mr_emit_result_t emit, void *emit_arg, void *user_arg) {
    (void)user_arg;
    int sum = 0;
    for (size_t i = 0; i < values_count; i++) {
        if (values[i].size == sizeof(int) && values[i].data != NULL) {
            sum += *(const int *)values[i].data;
        }
    }
    return emit(token, &sum, sizeof(sum), emit_arg);
}

/* TEST 1: Validazione parametri errati e gestione errori */
static void test_invalid_parameters(void) {
    printf("[TEST] Validazione parametri e gestione errori API...");

    mr_attr_t attr;
    TEST_ASSERT(mr_attr_init(&attr) == 0, "mr_attr_init deve riuscire");
    TEST_ASSERT(mr_attr_set_mapper_threads(&attr, 0) == -1, "thread mapper=0 deve fallire");
    TEST_ASSERT(mr_attr_set_reducer_threads(&attr, 0) == -1, "thread reducer=0 deve fallire");
    TEST_ASSERT(mr_attr_set_queue_size(&attr, 0) == -1, "queue_size=0 deve fallire");

    mr_t mr = NULL;
    TEST_ASSERT(mr_create(NULL, &attr, test_mapper_simple, test_reducer_sum, NULL) == -1, "mr NULL deve fallire");
    TEST_ASSERT(mr_create(&mr, &attr, NULL, test_reducer_sum, NULL) == -1, "mapper NULL deve fallire");
    TEST_ASSERT(mr_create(&mr, &attr, test_mapper_simple, NULL, NULL) == -1, "reducer NULL deve fallire");

    TEST_ASSERT(mr_create(&mr, &attr, test_mapper_simple, test_reducer_sum, NULL) == 0, "mr_create valido deve riuscire");
    TEST_ASSERT(mr_start(mr, "file_inesistente_12345.txt", "out.mro") == -1, "input inesistente deve fallire");

    mr_destroy(mr);
    mr_attr_destroy(&attr);
    printf(" SUPERATO\n");
}

/* TEST 2: Casi limite su righe e file (vuoto, righe vuote, riga singola, no newline finale) */
static void test_edge_cases(void) {
    printf("[TEST] Casi limite di input (file vuoto, righe vuote, no newline)...");

    system("mkdir -p /tmp/test_mr_edge");
    create_file("/tmp/test_mr_edge/empty.txt", "", 0);
    create_file("/tmp/test_mr_edge/blank_lines.txt", "\n\n\n", 3);
    create_file("/tmp/test_mr_edge/single_no_nl.txt", "Alpha Beta", 10);
    create_file("/tmp/test_mr_edge/multi.txt", "Alpha\n\nBeta Gamma\n", 18);

    mr_attr_t attr;
    mr_attr_init(&attr);
    mr_attr_set_log_file(&attr, "/tmp/test_mr_edge/edge.log");

    mr_t mr;
    mr_create(&mr, &attr, test_mapper_simple, test_reducer_sum, NULL);

    int res = mr_start(mr, "/tmp/test_mr_edge", "/tmp/test_mr_edge/out.mro");
    TEST_ASSERT(res == 0, "mr_start deve terminare con successo sui casi limite");

    mr_destroy(mr);
    mr_attr_destroy(&attr);
    printf(" SUPERATO\n");
}

/* TEST 3: Opacità dati binari (processed_token e result contenenti byte nulli e byte arbitrari) */
static int binary_mapper(const mr_file_line_t *line, mr_emit_pair_t emit, void *emit_arg, void *user_arg) {
    (void)user_arg;
    if (line->line_len == 0) return 0;
    /* Valore opaco con byte nullo '\0' e byte 0xFF */
    unsigned char bin_val[4] = {0x00, 0xFF, 0x00, 0x42};
    return emit("BinKey", bin_val, sizeof(bin_val), emit_arg);
}

static int binary_reducer(const char *token, const mr_value_t *values, size_t values_count,
                          mr_emit_result_t emit, void *emit_arg, void *user_arg) {
    (void)user_arg;
    TEST_ASSERT(values_count > 0, "Almeno un valore binario atteso");
    for (size_t i = 0; i < values_count; i++) {
        TEST_ASSERT(values[i].size == 4, "Dimensione valore opaco deve essere 4");
        const unsigned char *b = (const unsigned char *)values[i].data;
        TEST_ASSERT(b[0] == 0x00 && b[1] == 0xFF && b[2] == 0x00 && b[3] == 0x42,
                    "Integrità byte opachi con byte nullo preservata");
    }
    /* Risultato finale contenente anch'esso byte nulli */
    unsigned char final_bin[5] = {0xDE, 0x00, 0xAD, 0x00, 0xEF};
    return emit(token, final_bin, sizeof(final_bin), emit_arg);
}

static void test_binary_opacity(void) {
    printf("[TEST] Opacità completa dei dati (byte nulli e dati binari opachi)...");

    system("mkdir -p /tmp/test_mr_bin");
    create_file("/tmp/test_mr_bin/data.txt", "line1\nline2\n", 12);

    mr_attr_t attr;
    mr_attr_init(&attr);
    mr_attr_set_log_file(&attr, "/tmp/test_mr_bin/bin.log");

    mr_t mr;
    mr_create(&mr, &attr, binary_mapper, binary_reducer, NULL);

    int res = mr_start(mr, "/tmp/test_mr_bin/data.txt", "/tmp/test_mr_bin/out.mro");
    TEST_ASSERT(res == 0, "mr_start con valori binari deve riuscire");

    /* Verifichiamo che il record contenga il risultato binario esatto */
    int fd = open("/tmp/test_mr_bin/out.mro", O_RDONLY);
    TEST_ASSERT(fd >= 0, "Apertura output binario");
    mr_output_record_hdr_t hdr;
    TEST_ASSERT(mr_readn(fd, &hdr, sizeof(hdr)) == sizeof(hdr), "Lettura header");
    TEST_ASSERT(hdr.token_len == 6, "Token 'BinKey' lunghezza 6");
    TEST_ASSERT(hdr.result_len == 5, "Result lunghezza 5");

    char tok[7];
    mr_readn(fd, tok, 6);
    tok[6] = '\0';
    TEST_ASSERT(strcmp(tok, "BinKey") == 0, "Token corretto");

    unsigned char res_buf[5];
    mr_readn(fd, res_buf, 5);
    TEST_ASSERT(res_buf[0] == 0xDE && res_buf[1] == 0x00 && res_buf[2] == 0xAD &&
                res_buf[3] == 0x00 && res_buf[4] == 0xEF, "Byte del risultato verificati con successo");
    close(fd);

    mr_destroy(mr);
    mr_attr_destroy(&attr);
    printf(" SUPERATO\n");
}

/* TEST 4: Determinismo tra esecuzioni multiple concorrenti */
static void test_determinism(void) {
    printf("[TEST] Determinismo e riproducibilità bit-a-bit tra esecuzioni...");

    system("mkdir -p /tmp/test_mr_det");
    const char *text = "Zeta Alpha Gamma Beta Delta Epsilon Zeta Beta Gamma Alpha Zeta\n";
    create_file("/tmp/test_mr_det/input.txt", text, strlen(text));

    mr_attr_t attr;
    mr_attr_init(&attr);
    mr_attr_set_mapper_threads(&attr, 4);
    mr_attr_set_reducer_threads(&attr, 4);
    mr_attr_set_queue_size(&attr, 8);
    mr_attr_set_log_file(&attr, "/tmp/test_mr_det/det.log");

    mr_t mr1, mr2, mr3;
    mr_create(&mr1, &attr, test_mapper_simple, test_reducer_sum, NULL);
    mr_create(&mr2, &attr, test_mapper_simple, test_reducer_sum, NULL);
    mr_create(&mr3, &attr, test_mapper_simple, test_reducer_sum, NULL);

    TEST_ASSERT(mr_start(mr1, "/tmp/test_mr_det/input.txt", "/tmp/test_mr_det/out1.mro") == 0, "Run 1 ok");
    TEST_ASSERT(mr_start(mr2, "/tmp/test_mr_det/input.txt", "/tmp/test_mr_det/out2.mro") == 0, "Run 2 ok");
    TEST_ASSERT(mr_start(mr3, "/tmp/test_mr_det/input.txt", "/tmp/test_mr_det/out3.mro") == 0, "Run 3 ok");

    TEST_ASSERT(files_are_identical("/tmp/test_mr_det/out1.mro", "/tmp/test_mr_det/out2.mro"),
                "out1.mro e out2.mro devono essere identici");
    TEST_ASSERT(files_are_identical("/tmp/test_mr_det/out2.mro", "/tmp/test_mr_det/out3.mro"),
                "out2.mro e out3.mro devono essere identici");

    mr_destroy(mr1);
    mr_destroy(mr2);
    mr_destroy(mr3);
    mr_attr_destroy(&attr);
    printf(" SUPERATO\n");
}

/* TEST 5: Hashing deterministico personalizzato (Addendum) */
static size_t custom_test_hash(const char *token, size_t token_len, void *user_arg) {
    (void)user_arg;
    size_t h = 0;
    for (size_t i = 0; i < token_len; i++) {
        h = h * 31 + (unsigned char)token[i];
    }
    return h;
}

static void test_addendum_hash(void) {
    printf("[TEST] Funzione hash personalizzata (Addendum)...");

    system("mkdir -p /tmp/test_mr_hash");
    create_file("/tmp/test_mr_hash/in.txt", "one two three one two three four\n", 33);

    mr_attr_t attr;
    mr_attr_init(&attr);
    mr_attr_set_hash_function(&attr, custom_test_hash, NULL);
    mr_attr_set_log_file(&attr, "/tmp/test_mr_hash/hash.log");

    mr_t mr;
    mr_create(&mr, &attr, test_mapper_simple, test_reducer_sum, NULL);

    TEST_ASSERT(mr_start(mr, "/tmp/test_mr_hash/in.txt", "/tmp/test_mr_hash/out.mro") == 0, "mr_start con hash custom");

    mr_destroy(mr);
    mr_attr_destroy(&attr);
    printf(" SUPERATO\n");
}

/* Helper per conteggiare record e verificare valori in un file .mro */
static int get_record_count_and_check(const char *mro_path, const char *check_tok, int expected_val) {
    int fd = open(mro_path, O_RDONLY);
    if (fd < 0) return -1;

    int total_records = 0;
    int check_matched = (check_tok == NULL) ? 1 : 0;

    while (1) {
        mr_output_record_hdr_t hdr;
        ssize_t r = mr_readn(fd, &hdr, sizeof(hdr));
        if (r <= 0) break;

        char tok[256];
        if (hdr.token_len >= (int)sizeof(tok)) {
            close(fd);
            return -1;
        }
        mr_readn(fd, tok, (size_t)hdr.token_len);
        tok[hdr.token_len] = '\0';

        int val = 0;
        if (hdr.result_len == sizeof(int)) {
            mr_readn(fd, &val, sizeof(int));
        } else {
            char skip_buf[128];
            size_t left = (size_t)hdr.result_len;
            while (left > 0) {
                size_t chunk = (left < sizeof(skip_buf)) ? left : sizeof(skip_buf);
                mr_readn(fd, skip_buf, chunk);
                left -= chunk;
            }
        }

        if (check_tok != NULL && strcmp(tok, check_tok) == 0 && val == expected_val) {
            check_matched = 1;
        }

        total_records++;
    }

    close(fd);
    return check_matched ? total_records : -1;
}

/* TEST 6: Stress test su larga scala (5 file, 10.000 righe, 100.000 token, 8 mapper e 8 reducer thread) */
static void test_stress_workload(void) {
    printf("[TEST LARGA SCALA] Stress test concorrente (5 file, 10.000 righe, 100.000 token)...");

    system("mkdir -p /tmp/test_mr_stress/input");

    /* Generazione di 5 file con 2.000 righe ciascuno (10 parole per riga) */
    const char *line_sample = "alpha beta gamma delta epsilon zeta eta theta iota kappa\n";
    for (int f = 1; f <= 5; f++) {
        char filename[128];
        snprintf(filename, sizeof(filename), "/tmp/test_mr_stress/input/file_%d.txt", f);
        FILE *fp = fopen(filename, "w");
        TEST_ASSERT(fp != NULL, "Creazione file di stress");
        for (int l = 0; l < 2000; l++) {
            fputs(line_sample, fp);
        }
        fclose(fp);
    }

    /* Esecuzione con 8 mapper thread, 8 reducer thread e coda piccola per forzare contesa */
    mr_attr_t attr;
    mr_attr_init(&attr);
    mr_attr_set_mapper_threads(&attr, 8);
    mr_attr_set_reducer_threads(&attr, 8);
    mr_attr_set_queue_size(&attr, 16);
    mr_attr_set_log_file(&attr, "/tmp/test_mr_stress/stress.log");

    mr_t mr1, mr2;
    mr_create(&mr1, &attr, test_mapper_simple, test_reducer_sum, NULL);
    mr_create(&mr2, &attr, test_mapper_simple, test_reducer_sum, NULL);

    /* Due run indipendenti per verificare correttezza e determinismo */
    TEST_ASSERT(mr_start(mr1, "/tmp/test_mr_stress/input", "/tmp/test_mr_stress/out1.mro") == 0, "Stress Run 1");
    TEST_ASSERT(mr_start(mr2, "/tmp/test_mr_stress/input", "/tmp/test_mr_stress/out2.mro") == 0, "Stress Run 2");

    /* Verifica determinismo bit-a-bit sotto carico elevato */
    TEST_ASSERT(files_are_identical("/tmp/test_mr_stress/out1.mro", "/tmp/test_mr_stress/out2.mro"),
                "Stress output identici bit-a-bit");

    /* Verifica conteggio esatto: 10 parole distinte, ciascuna presente esattamente 10.000 volte */
    int records = get_record_count_and_check("/tmp/test_mr_stress/out1.mro", "alpha", 10000);
    TEST_ASSERT(records == 10, "Esattamente 10 token distinti attesi");
    TEST_ASSERT(get_record_count_and_check("/tmp/test_mr_stress/out1.mro", "kappa", 10000) == 10,
                "Conteggio kappa atteso 10000");

    mr_destroy(mr1);
    mr_destroy(mr2);
    mr_attr_destroy(&attr);
    printf(" SUPERATO (100.000 token elaborati)\n");
}

/* TEST 7: Elevata cardinalità di token distinti (3.000 chiavi uniche ripetute) */
static void test_large_distinct_tokens(void) {
    printf("[TEST LARGA SCALA] Alta cardinalità (3.000 token unici distinti)...");

    system("mkdir -p /tmp/test_mr_tokens");
    FILE *fp = fopen("/tmp/test_mr_tokens/dataset.txt", "w");
    TEST_ASSERT(fp != NULL, "Creazione file dataset tokens");

    /* Scriviamo 3.000 chiavi distinte (key0000, key0001, ...), ripetute 3 volte */
    for (int rep = 0; rep < 3; rep++) {
        for (int k = 0; k < 3000; k++) {
            fprintf(fp, "key%04d ", k);
            if (k % 20 == 19) {
                fputc('\n', fp);
            }
        }
        fputc('\n', fp);
    }
    fclose(fp);

    mr_attr_t attr;
    mr_attr_init(&attr);
    mr_attr_set_mapper_threads(&attr, 4);
    mr_attr_set_reducer_threads(&attr, 4);
    mr_attr_set_queue_size(&attr, 64);
    mr_attr_set_log_file(&attr, "/tmp/test_mr_tokens/tokens.log");

    mr_t mr;
    mr_create(&mr, &attr, test_mapper_simple, test_reducer_sum, NULL);

    TEST_ASSERT(mr_start(mr, "/tmp/test_mr_tokens/dataset.txt", "/tmp/test_mr_tokens/out.mro") == 0,
                "mr_start con 3000 token distinti");

    /* Verifica che ci siano esattamente 3.000 token e che ciascuno abbia conteggio 3 */
    int records = get_record_count_and_check("/tmp/test_mr_tokens/out.mro", "key0042", 3);
    TEST_ASSERT(records == 3000, "Attesi esattamente 3000 token unici nell'output");

    mr_destroy(mr);
    mr_attr_destroy(&attr);
    printf(" SUPERATO (3.000 chiavi uniche verificate)\n");
}

/* TEST 8: Righe molto lunghe (superamento limiti buffer pipe Linux ~64KB) */
static void test_very_long_lines(void) {
    printf("[TEST LARGA SCALA] Righe molto lunghe (>64KB per riga, streaming pipe)...");

    system("mkdir -p /tmp/test_mr_longlines");
    FILE *fp = fopen("/tmp/test_mr_longlines/long.txt", "w");
    TEST_ASSERT(fp != NULL, "Creazione file con righe giganti");

    /* 4 righe giganti, ciascuna contenente circa 15.000 parole (~90 KB a riga) */
    for (int l = 0; l < 4; l++) {
        for (int w = 0; w < 15000; w++) {
            fputs("giantline ", fp);
        }
        fputc('\n', fp);
    }
    fclose(fp);

    mr_attr_t attr;
    mr_attr_init(&attr);
    mr_attr_set_mapper_threads(&attr, 4);
    mr_attr_set_reducer_threads(&attr, 4);
    mr_attr_set_queue_size(&attr, 32);
    mr_attr_set_log_file(&attr, "/tmp/test_mr_longlines/long.log");

    mr_t mr;
    mr_create(&mr, &attr, test_mapper_simple, test_reducer_sum, NULL);

    TEST_ASSERT(mr_start(mr, "/tmp/test_mr_longlines/long.txt", "/tmp/test_mr_longlines/out.mro") == 0,
                "mr_start con righe giganti streaming");

    /* Totale parole: 4 * 15.000 = 60.000 */
    int records = get_record_count_and_check("/tmp/test_mr_longlines/out.mro", "giantline", 60000);
    TEST_ASSERT(records == 1, "Atteso 1 solo token con conteggio 60000");

    mr_destroy(mr);
    mr_attr_destroy(&attr);
    printf(" SUPERATO (righe giganti gestite in streaming)\n");
}

int main(void) {
    printf("====================================================\n");
    printf(" AVVIO SUITE DI TEST AUTOMATIZZATI libmr (COMPLETA) \n");
    printf("====================================================\n");

    /* Test funzionali e di specifica */
    test_invalid_parameters();
    test_edge_cases();
    test_binary_opacity();
    test_determinism();
    test_addendum_hash();

    /* Test su larga scala e carichi intensivi */
    test_stress_workload();
    test_large_distinct_tokens();
    test_very_long_lines();

    printf("====================================================\n");
    printf(" TUTTI I TEST SONO STATI SUPERATI CON SUCCESSO!     \n");
    printf("====================================================\n");
    return 0;
}

