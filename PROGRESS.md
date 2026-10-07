# Stato di Avanzamento Progetto (`PROGRESS.md`)

## 1. Obiettivo Generale
Implementazione completa del framework C concorrente `libmr` (Progetto di Laboratorio 2 A, a.a. 2025-26), basato su:
- Pipeline a 3 processi (Main -> Mapper -> Reducer -> Main) con `fork()`, `pipe()`, `dup2()`, `waitpid()`.
- Thread nativi C11 (`<threads.h>`) sia nel processo Mapper sia nel processo Reducer.
- Esclusione tassativa di pthreads, exec, socket e memoria condivisa.
- Protocollo binario esplicito basato su lunghezze su pipe.
- Opacità completa di valori intermedi e risultati.
- Output deterministico ordinato lessicograficamente per token.
- Logging sincronizzato concorrente multiprocesso e multithread.
- Supporto completo ai requisiti dell'Addendum (hashing deterministico, directory ricorsiva, istanze multiple, statistiche).
- Build system con `Makefile` (`make`, `make test`, `make clean`), esempi e suite di test automatizzati.

## 2. Attività Completate
- [x] Studio e analisi approfondita delle specifiche tecniche e dei vincoli tassativi da `Testo.pdf`.
- [x] Redazione dell'analisi formale dei requisiti e del meta-prompt in `prompt.md`.
- [x] Creazione di `include/mr.h` con API pubblica, tipi opachi, strutture configurazione ed estensioni Addendum.
- [x] Implementazione di `src/mr_common.h` e `src/mr_common.c` con primitive I/O (`readn`, `writen`), validazione header, funzione hash default e logger thread/process-safe.
- [x] Implementazione di `src/mr_queue.h` e `src/mr_queue.c` con coda circolare bounded sincronizzata esclusivamente con `mtx_t` e `cnd_t` C11.
- [x] Implementazione di `src/mr_mapper.h` e `src/mr_mapper.c` con processo mapper, reader C11, worker C11, atomicità scrittura su pipe e gestione pulita EOF.
- [x] Implementazione di `src/mr_reducer.h` e `src/mr_reducer.c` con processo reducer, reader C11, raggruppamento per chiave, partizionamento tramite hash deterministico e worker C11.
- [x] Implementazione di `src/mr_api.c` con gestione della pipeline a 3 processi, `fork()`, `dup2()`, chiusura descrittori anti-deadlock, lettura file con ordinamento lessicografico, ordinamento deterministico output e statistiche.
- [x] Creazione di `examples/word_count.c` e dell'utility ausiliaria `examples/dump_output.c`.
- [x] Creazione di `tests/test_suite.c` con test automatizzati per parametri invalidi, casi limite, opacità byte binari, determinismo bit-a-bit e hash Addendum.
- [x] Creazione di `Makefile` (`all`, `test`, `clean`) con flag rigidi (`-std=c11 -Wall -Wextra -Werror -pedantic -O2`) e `README.md`.
- [x] Refactoring del codice in stile universitario triennale: rimozione di funzioni `mem*` (`memcmp`, `memcpy`, `memset`), sostituzione con cicli standard o inizializzazioni chiare.
- [x] Rimozione totale di qualsiasi assegnamento manuale a `errno` in favore del semplice ritorno di `-1` in caso di errore.
- [x] Aggiunta di test intensivi su larga scala: stress test con 100.000 token e 8 thread concorrenti, alta cardinalità con 3.000 token unici distinti, e streaming con righe giganti (>64KB).
- [x] Compilazione con successo dell'intera libreria statica `libmr.a`, degli esempi e superamento al 100% dell'intera suite di test automatizzata (inclusi i test su larga scala).
- [x] Documentazione di tutte le funzioni nei file `.c` (requisito "codice commentato in modo adeguato" della Sezione 17 di `Testo.pdf`): intestazione di file, commenti di funzione con scopo/parametri/valore di ritorno e commenti alle strutture dati, in `src/mr_api.c`, `src/mr_common.c`, `src/mr_queue.c`, `src/mr_mapper.c`, `src/mr_reducer.c`, `examples/word_count.c`, `examples/dump_output.c`, `tests/test_suite.c`.

## 3. Stato Corrente (Cosa si sta facendo)
- Implementazione completata, semplificata secondo lo standard universitario triennale e testata con successo.
- Documentazione delle funzioni completata in tutti i file `.c`; build con `-Wall -Wextra -Werror -pedantic` pulita e `make test` al 100%.
- Progetto pronto per la consegna o per ulteriori verifiche / redazione della relazione tecnica finale.

## 4. Prossimi Passi
- [ ] (Opzionale) Redazione della relazione tecnica (PDF massimo 10 pagine) se richiesta per la consegna d'esame.
- [ ] Creazione eventuale del pacchetto `.zip` per la consegna finale.

## 5. Note Tecniche & Decisioni
- Rispetto totale di tutti i vincoli tassativi: zero pthread, zero exec, zero shm, zero socket.
- Utilizzo esclusivo di thread C11 (`<threads.h>`) e processi con `fork()`, `pipe()`, `dup2()`, `waitpid()`.
- Codice lineare e idiomatico per un esame triennale: cicli for espliciti per la copia/confronto byte, assenza di funzioni oscure, gestione naturale degli errori.
- Determinismo perfetto verificato su corse multiple bit-a-bit identiche.


