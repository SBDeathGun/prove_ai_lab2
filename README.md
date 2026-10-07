# Framework MapReduce `libmr` (Laboratorio 2 A, a.a. 2025-26)

Libreria in linguaggio C per l'analisi concorrente di file di testo basata sul paradigma **MapReduce** su singola macchina Linux, sviluppata nel pieno rispetto dei vincoli architetturali e dei contratti della specifica di progetto.

---

## 1. Architettura del Sistema

Il framework organizza la computazione come una pipeline a **3 processi** cooperanti e multithread:

```
[ Processo Principale (Main) ]
             |
             | Pipe anonima: righe serializzate (main_to_mapper)
             v
  [ Processo Mapper (C11) ]
             |
             | Pipe anonima: coppie <token, valore> serializzate (mapper_to_reducer)
             v
 [ Processo Reducer (C11) ]
             |
             | Pipe anonima: risultati serializzati (reducer_to_main)
             v
[ Processo Principale (Main) ]  -->  Scrittura file di output deterministico
```

### 1.1 Vincoli Tecnologici Rispettati
- **Processi**: creati esclusivamente con `fork()` e monitorati con `waitpid()`.
- **Nessun `exec()`**: i callback `mapper` e `reducer` sono invocati nello spazio di memoria ereditato dal processo genitore.
- **IPC**: unicamente pipe anonime unidirezionali (`pipe()`) e ridirezione dello standard I/O tramite `dup2()`.
- **Nessuna memoria condivisa tra processi** e **nessun socket**.
- **Thread C11**: multithreading conforme allo standard C11 (`<threads.h>`), con primitive `thrd_create`, `thrd_join`, `mtx_t`, `cnd_t`. Nessun uso diretto della libreria POSIX Threads (`<pthread.h>`).
- **Sincronizzazione di Log**: sincronizzazione multi-processo tramite lock su file (`flock`) e sincronizzazione tra thread interni tramite `mtx_t`.

---

## 2. Dettagli Architetturali e Anti-Deadlock

1. **Creazione Processi & Thread**:
   I processi Mapper e Reducer vengono creati con `fork()` prima dell'inizializzazione di qualsiasi thread C11.
2. **Propagazione di EOF (Chiusura Pipe)**:
   - Ogni processo chiude tempestivamente tutti i descrittori delle pipe non utilizzati.
   - Il Main chiude il lato di scrittura della pipe verso il Mapper quando ha terminato di inviare le righe.
   - Il Mapper riceve `EOF` su `stdin`, conclude lo svuotamento della coda dei worker C11, effettua il `thrd_join` e solo allora chiude il proprio `stdout` (pipe verso il Reducer).
   - Il Reducer riceve `EOF` su `stdin`, completa il raggruppamento per chiave, avvia i worker C11 per invocare la callback `reducer`, attende la terminazione dei worker e chiude il proprio `stdout`.
   - Il Main riceve `EOF` su `stdin`, ordina i risultati in modo deterministico e scrive il file finale.

---

## 3. Protocollo su Pipe e Formato Record

Tutti i messaggi scambiati su pipe utilizzano un protocollo binario esplicito basato su lunghezze (`int`):
- **Righe (Main -> Mapper)**: `file_name_len`, `line_len`, `line_number` seguiti dai rispettivi byte.
- **Coppie Intermedie (Mapper -> Reducer)**: `token_len`, `value_len`, seguiti dai byte del token (senza `\0`) e dai byte opachi del valore.
- **Risultati Finali (Reducer -> Main)**: `token_len`, `result_len`, seguiti dai byte del token e dai byte opachi del risultato.

### Validazione e Opacità
- Ogni lunghezza è validata (`len >= 0` e `len <= LIMITE_MASSIMO`).
- I valori `processed_token` e `result` sono trattati come sequenze opache di byte: possono contenere byte nulli (`'\0'`) e caratteri non stampabili. Il framework non esegue mai funzioni di stringa (`strlen`, `strcmp`, `strcpy`) su tali dati.

---

## 4. Compilazione e Test

Il progetto include un `Makefile` conforme a tutte le specifiche universitarie:

### Compilazione della Libreria e degli Esempi
```bash
make
```
Genera la libreria statica `libmr.a` e gli eseguibili `examples/word_count` e `examples/dump_output`.

### Esecuzione della Suite di Test
```bash
make test
```
Esegue i test automatici che verificano:
- Rifiuto dei parametri non validi (`EINVAL`).
- Gestione di casi limite: file vuoti, righe vuote, riga singola, ultima riga senza `\n`.
- Opacità di valori intermedi e finali con byte nulli (`\0`).
- Determinismo bit-a-bit e riproducibilità tra esecuzioni multiple.
- Funzione di hashing personalizzata (Addendum).

### Pulizia
```bash
make clean
```

---

## 5. Esempi d'Uso

### Conteggio Parole (`word_count`)
```bash
./examples/word_count <file_o_cartella_input> <file_output.mro> [file_log]
```

### Ispezione Output Binario (`dump_output`)
```bash
./examples/dump_output <file_output.mro>
```

---

## 6. Funzionalità dell'Addendum Implementate
- **Scansione Ricorsiva**: supporto alla lettura ricorsiva di directory annidate di input con ordinamento lessicografico.
- **Partizionamento Hashing Deterministico**: supporto alla configurazione di funzioni hash utente (`mr_attr_set_hash_function`) per instradare i token verso i thread worker del Reducer.
- **Statistiche di Esecuzione**: generazione automatica del report di statistiche (`<output>.stats`) con tempi di calcolo, righe lette e risultati emessi.
- **Isolamento delle Istanze**: assenza totale di variabili globali/statiche condivise; supporto all'esecuzione concorrente di istanze indipendenti `mr_t`.
