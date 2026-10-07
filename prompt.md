In qualità di Specialista di Ingegneria del Software e Sistemi Operativi, ho effettuato un’analisi approfondita e rigorosa del documento di specifica Testo.pdf (Progetto di Laboratorio
    2 A – Framework per analisi di file di testo adottando il paradigma MapReduce, a.a. 2025-26).
    Di seguito trovi:
    1. La specifica formale e sistematica dei requisiti, suddivisa per aree di responsabilità, vincoli architetturali, contratti d'interfaccia e invarianti di correttezza (tassativi).
    2. Il Meta-Prompt ingegneristico di implementazione, progettato per guidare la realizzazione completa, robusta e priva di difetti dell'intero progetto conforme agli standard
    universitari.
    ──────
     PARTE 1: ANALISI FORMALE DEI REQUISITI (Software Requirements Specification)
    ## 1. Visione del Sistema e Obiettivi
  
    Realizzazione di una libreria/framework in linguaggio C denominata libmr (con header pubblico include/mr.h e libreria statica libmr.a) che implementa un motore di computazione
    concorrente basato sul paradigma MapReduce su singola macchina (ambiente Linux Ubuntu 24.04).
    Il framework astrae la concorrenza, la gestione dei processi, la sincronizzazione dei thread e la comunicazione IPC tramite pipe, esponendo all'applicazione utente una semplice
    interfaccia funzionale basata su callback (mapper e reducer).
    ──────
    ## 2. Vincoli Tecnologici e Primitivi Tassativi
    I vincoli sulle primitive di sistema sono stringenti:
  
    • Primitivi ESCLUSI / VIETATI tassativamente:
        • Nessun socket (di qualsiasi dominio).
        • Nessuna memoria condivisa tra processi (shmget, POSIX shared memory shm_open, mmap(MAP_SHARED), ecc.).
        • Nessun uso di funzioni della famiglia exec() (execl, execv, ecc.): mapper e reducer sono callback C all'interno del medesimo binario; i processi figli ereditano lo spazio di
        indirizzamento tramite fork().
        • Nessun uso di thread POSIX (pthreads): è severamente vietato includere <pthread.h> e utilizzare pthread_create, pthread_mutex_t, pthread_cond_t, ecc.
    • Primitivi AMMESSI e OBBLIGATORI:
        • Processi: creati esclusivamente con fork(), gestiti con waitpid().
        • IPC: esclusivamente pipe anonime unidirezionali (pipe()), con ridirezione tramite dup2().
        • Thread: multithreading nativo C11 tramite <threads.h> (thrd_create, thrd_join, thrd_t, mtx_t, cnd_t, conformi alla firma thrd_start_t: int (*)(void *)).
        • Sincronizzazione inter-processo: consentito l'uso di semafori POSIX (<semaphore.h>) o lock su file (fcntl/flock) esclusivamente per il logging concorrente.
    ──────
    ## 3. Architettura dei Processi e Topologia IPC
  
    Il sistema è strutturato come una pipeline lineare composta da 3 processi distinti:
  
      [ Processo Principale (Main) ]
                   |
                   | Pipe: righe serializzate (main_to_mapper)
                   v
        [ Processo Mapper (C11) ]
                   |
                   | Pipe: coppie <token, valore> serializzate (mapper_to_reducer)
                   v
       [ Processo Reducer (C11) ]
                   |
                   | Pipe: risultati serializzati (reducer_to_main)
                   v
      [ Processo Principale (Main) ] (Scrittura file di output deterministico)
  
    ### Invarianti di Gestione Descrittori e Segnalazione EOF
    • Ordine di creazione: fork() deve essere invocata dal processo principale prima che venga avviato qualsiasi thread C11 (evitare fork in processi multithread).
    • Ridirezione dup2():
        • Nel processo Mapper: stdin collegato alla pipe dal Main; stdout collegato alla pipe verso il Reducer.
        • Nel processo Reducer: stdin collegato alla pipe dal Mapper; stdout collegato alla pipe verso il Main.
    • Chiusura dei Descrittori (Anti-Deadlock):
        • Ogni processo deve chiudere immediatamente tutti i lati di lettura/scrittura delle pipe non utilizzati.
        • La segnalazione di fine dati avviene esclusivamente tramite ricezione di EOF alla chiusura della pipe di scrittura, senza messaggi sentinella o protocolli speciali ad-hoc.
        • Nel Mapper: la pipe verso il Reducer (stdout) deve essere chiusa soltanto una volta, dal thread coordinatore, dopo che tutti i worker thread del Mapper sono terminati.
        • Nel Reducer: la pipe verso il Main (stdout) deve essere chiusa solo dopo il completamento di tutti i worker del Reducer e la scrittura di tutti i risultati.
  
    ──────
    ## 4. Architettura Interna della Concorrenza (Thread C11)
  
    ### 4.1 Processo Mapper
  
    • Thread Reader: legge dal proprio stdin le righe serializzate inviate dal Main e le inserisce in una coda FIFO bounded protetta da mtx_t e cnd_t. All'EOF su stdin, marca la coda come
    terminata e risveglia i worker.
    • Worker Threads (mapper_threads): estraggono le righe dalla coda, costruiscono la struttura locale mr_file_line_t e invocano la callback mapper.
    • Funzione di Emissione (mr_emit_pair_t): invocata dal worker durante il mapping; deve serializzare e scrivere la coppia ⟨token, processed_token⟩ su stdout (pipe verso il reducer). La
    scrittura sulla pipe deve essere protetta da un mutex per garantire l'atomicità di ogni singolo messaggio (nessuna interleaving di byte).
    • Copia Dati: i dati passati a emit devono essere interamente copiati nel buffer di invio prima che la funzione emit ritorni.
  
    ### 4.2 Processo Reducer
    • Thread Reader & Raggruppamento: legge le coppie ⟨token, processed_token⟩ dallo stdin.
    • Fase di Raggruppamento: raccoglie tutti i valori associati a ciascun token distinto in memoria dinamica (hash table / albero bilanciato).
    • Invocazione Unica per Token: la callback reducer non deve mai essere invocata singolarmente per ogni coppia, ma una sola volta per ciascun token distinto, passando l'array completo
    mr_value_t *values con cardinalità values_count.
    • Worker Threads (reducer_threads): elaborano i gruppi formati invocando la callback applicativa reducer.
    • Funzione di Emissione (mr_emit_result_t): scrive in modo atomico e sincronizzato i risultati serializzati verso stdout (pipe verso il Main). Anche in questo caso, copia immediata del
    risultato prima del ritorno di emit.
    ──────
    ## 5. Protocollo Binario su Pipe e Robustezza I/O
    Il protocollo di comunicazione inter-processo deve essere esplicito e basato su lunghezze (TLV o header fisso + payload):
  
    • Header Coppie Intermedie:
      typedef struct {
          int token_len;
          int value_len;
      } mr_pair_header_t;
  
    • Header Righe (Main -> Mapper): deve trasmettere lunghezza nome file, lunghezza contenuto riga, numero riga (partendo da 1), seguiti dai byte effettivi del nome file e della riga.
    • Header Risultati (Reducer -> Main): lunghezza token, lunghezza risultato, seguiti dai rispettivi payload.
    • Validazione Tassativa delle Lunghezze:
        • I campi lunghezza negli header sono di tipo int.
        • Regola di validazione: ogni lunghezza ricevuta deve essere verificata rigorosamente prima dell'uso. Rifiutare valori negativi (< 0) o superiori a soglie massime
        configurate/ragionevoli prima del cast a size_t. Errori di protocollo devono causare abort pulito con diagnostica.
    • Gestione I/O Parziale:
        • Implementazione obbligatoria e robusta di primitive ausiliarie readn(int fd, void *buf, size_t n) e writen(int fd, const void *buf, size_t n) che gestiscano correttamente
        letture/scritture parziali e segnali (EINTR).
  
    ──────
    ## 6. Trattamento dei Dati: Contratti di Opacità e Formato
    • Token (Chiave):
        • Sequenza non vuota di soli caratteri alfanumerici ASCII ([A-Za-z0-9]).
        • Nell'API C viene esposta come stringa terminata da '\0'.
        • Il carattere '\0' non fa parte della dimensione logica del token e non viene conteggiato in token_len sul canale di comunicazione.
        • Il framework può utilizzare funzioni di confronto stringhe (strcmp) esclusivamente per ordinare/confrontare i token.
    • Processed Token & Result (Valori):
        • Assoluta opacità binaria: sequenze arbitrarie di byte di dimensione nota (size_t).
        • Possono contenere byte nulli ('\0') e byte non stampabili.
        • Divieto assoluto: il framework non deve mai applicare funzioni come strlen, strcpy, strcmp o formattatori come printf("%s") sui valori intermedi o finali.
        • Se la dimensione è 0, il puntatore può essere NULL.
    ──────
    ## 7. Input, Output e Requisiti di Determinismo
    ### Gestione Input
  
    • Supporto per singolo file regolare o per directory contenente file regolari.
    • Se l'input è una directory, tutti i file regolari direttamente contenuti devono essere letti in ordine lessicografico rigoroso rispetto al loro nome.
    • Unità logica di elaborazione per il Mapper: la singola riga di testo (senza il carattere di newline \n finale).
    • Gestione di casi limite:
        • File vuoti (nessun errore, zero righe).
        • Righe vuote (riga con line_len == 0).
        • File a riga singola.
        • Ultima riga non terminata da \n.
    ### Formato File di Output e Determinismo
    • L'output deve essere scritto direttamente dal framework nel file di destinazione (prodotto dal processo Main leggendo dalla pipe del Reducer).
    • Formato a record binari con lunghezze esplicite:
        • [lunghezza_token] [byte_token] [lunghezza_risultato] [byte_risultato]
    • Determinismo: a parità di input, funzioni applicative e parametri, il file di output binario deve risultare bit-a-bit identico tra esecuzioni diverse.
    • Per garantire il determinismo, i risultati finali devono essere ordinati lessicograficamente per token prima della scrittura finale nel file di output. L'ordine relativo per più
    risultati con lo stesso token deve essere prefissato e documentato (es. ordine di emissione).
    ──────
    ## 8. Tracciamento e Logging Concorrente
    • File di log configurabile (default mr.log), impostabile con mr_attr_set_log_file().
    • Formato unificato di riga:
    [timestamp] [processo] [thread] [evento] messaggio
    • Eventi minimi obbligatori da tracciare:
        1. Creazione delle pipe.
        2. Creazione dei processi Mapper e Reducer.
        3. Avvio e terminazione dei thread C11.
        4. Apertura e chiusura dei file di input e output.
        5. Conteggio righe inviate al Mapper.
        6. Conteggio coppie prodotte dal Mapper.
        7. Conteggio token distinti raggruppati dal Reducer.
        8. Conteggio risultati finali prodotti.
        9. Errori rilevati.
    • Sincronizzazione: la scrittura sul file di log da processi differenti e thread concorrenti deve essere sincronizzata (utilizzando semafori POSIX con nome/condivisi o lock su file con
    fcntl/flock).
    ──────
    ## 9. Contratto dell'API Pubblica (include/mr.h)
  
    • Convenzione di ritorno: 0 in caso di successo, -1 in caso di errore (con impostazione corretta di errno).
    • mr_t: puntatore a tipo opaco struct mr.
    • mr_attr_t: struttura trasparente allocabile sullo stack dal chiamante.
    • mr_create(): esegue una copia interna immutabile degli attributi, rendendo modificabile/distruggibile mr_attr_t subito dopo la chiamata.
    • mr_start(): chiamata sincrona e bloccante. Esegue l'intera pipeline e ritorna solo a computazione conclusa e file di output completato.
    • mr_destroy(): deallocazione completa di tutte le risorse residue.
    ──────
    ## 10. Specifiche Addendum (Estensioni Opzionali / Tassative per Prove in Itinere)
    • Scansione ricorsiva delle sotto-directory di input.
    • Esecuzioni multiple indipendenti nello stesso processo senza interferenze tra diverse istanze di mr_t.
    • File separato di statistiche (tempi di esecuzione, righe lette, coppie prodotte, token distinti, risultati emessi).
    • Partizionamento deterministico per token: estensione di mr_attr_t con mr_hash_t e void *hash_arg (mr_attr_set_hash_function) per instradare i token verso i thread worker interni al
    Reducer.
    ──────
    ## 11. Organizzazione del Progetto e Consegna
  
    • Directory di progetto:
        • include/mr.h: header pubblico conforme al contratto.
        • src/: codice sorgente modulare (.c e header interni .h).
        • examples/: programma di esempio (Word Count) + utility ausiliaria per la stampa/decodifica del file di output binario.
        • tests/: batteria di test automatici (test unitari, di integrazione, stress test di concorrenza, gestione errori e casi limite).
        • Makefile: target obbligatori make, make test, make clean.
        • README: istruzioni di build, test ed esecuzione.
        • Relazione.pdf: documentazione tecnica (massimo 10 pagine).
    • Pacchetto finale: archivio .zip compilabile immediatamente con make.
    ──────
     PARTE 2: IL META-PROMPT PER L'IMPLEMENTAZIONE
  
    Di seguito è fornito il prompt dettagliato, auto-consistente e privo di ambiguità da sottomettere all'assistente di coding per realizzare l'intero progetto rispettando al 100% tutte le
    specifiche.
  
      Agisci come un Principal Software Engineer ed esperto di Sistemi Operativi Linux e programmazione di sistema in C (standard C11).
      Il tuo obiettivo è implementare in modo completo, robusto e conforme alle specifiche il progetto universitario "Framework per analisi di file di testo adottando il paradigma
    MapReduce" (libreria `libmr`).
  
      L'ambiente di riferimento è Linux Ubuntu 24.04 (GCC 13+, C11, POSIX 2008).
      TUTTI I REQUISITI DI SEGUITO RIPORTATI SONO TASSATIVI E NON AMMETTONO ECCEZIONI.
  
      ---
  
      ### 1. VINCOLI TECNOLOGICI E PROIBIZIONI ASSOLUTE
      1. È SEVERAMENTE VIETATO l'uso di:
         - Socket di qualunque tipologia.
         - Memoria condivisa tra processi (shmget, shm_open, mmap con MAP_SHARED, ecc.).
         - Qualsiasi funzione della famiglia exec() (execl, execv, execvp, ecc.).
         - La libreria POSIX Threads (<pthread.h>, pthread_create, pthread_mutex_t, pthread_cond_t, ecc.).
      2. È OBBLIGATORIO l'uso di:
         - Processi generati ESCLUSIVAMENTE tramite fork().
         - Pipeline IPC realizzata ESCLUSIVAMENTE tramite pipe anonime (pipe()) e ridirezione con dup2().
         - Multithreading realizzato ESCLUSIVAMENTE con i thread dello standard C11 (<threads.h>): thrd_create, thrd_join, thrd_t, mtx_t, cnd_t (start routine conforme a `int (*)(void
  *)`).
         - Gestione terminazione processi figli con waitpid().
         - Sincronizzazione inter-processo per il file di log tramite semafori POSIX (<semaphore.h>) o lock su file (fcntl/flock).
  
      ---
  
      ### 2. ARCHITETTURA DELLA PIPELINE IPC E GESTIONE DESCRITTORI
      La pipeline deve essere composta da 3 processi collegati in cascata da 3 pipe anonime:
         Main Process ---> [main_to_mapper] ---> Mapper Process ---> [mapper_to_reducer] ---> Reducer Process ---> [reducer_to_main] ---> Main Process
  
      Regole di instradamento e chiusura descrittori:
      1. I processi figli Mapper e Reducer devono essere creati con fork() PRIMA di creare qualsiasi thread C11 (nessuna fork in processi multithread).
      2. Nel Mapper: dup2() collega main_to_mapper[0] a STDIN_FILENO e mapper_to_reducer[1] a STDOUT_FILENO. Tutti gli altri descrittori di pipe non utilizzati devono essere immediatamente
    chiusi.
      3. Nel Reducer: dup2() collega mapper_to_reducer[0] a STDIN_FILENO e reducer_to_main[1] a STDOUT_FILENO. Tutti gli altri descrittori di pipe non utilizzati devono essere
    immediatamente chiusi.
      4. Nel Main: rimangono aperti SOLO main_to_mapper[1] (scrittura righe) e reducer_to_main[0] (lettura risultati). Tutti gli altri descrittori di pipe devono essere chiusi.
      5. Protocollo di terminazione EOF (ANTI-DEADLOCK):
         - La fine dei dati è segnalata SOLO dalla chiusura del descrittore di scrittura e conseguente EOF in lettura. NESSUN messaggio speciale di terminazione.
         - Il Main chiude main_to_mapper[1] quando ha terminato l'invio delle righe.
         - Il Mapper, ricevuto EOF su stdin, aspetta che tutti i suoi worker thread C11 abbiano terminato di processare la coda interna e SOLO ALLORA chiude STDOUT_FILENO
    (mapper_to_reducer[1]).
         - Il Reducer, ricevuto EOF su stdin, completa il raggruppamento per chiave, avvia i worker thread C11 per elaborare tutti i gruppi, aspetta la loro terminazione e SOLO ALLORA
    chiude STDOUT_FILENO (reducer_to_main[1]).
         - Il Main legge tutti i risultati da reducer_to_main[0] fino a EOF, produce il file di output e attende i figli con waitpid().
  
      ---
  
      ### 3. PROTOCOLLO BINARIO SU PIPE E ROBUSTEZZA I/O
      1. La comunicazione su pipe deve utilizzare un protocollo a pacchetti con lunghezze esplicite (int).
      2. Definire e usare funzioni ausiliarie:
         - `ssize_t readn(int fd, void *buf, size_t n)`
         - `ssize_t writen(int fd, const void *buf, size_t n)`
         gestendo loop di trasferimento completi, letture/scritture parziali e interruzioni da segnali (EINTR).
      3. Validazione di sicurezza degli header:
         - Ogni lunghezza ricevuta (es. token_len, value_len, line_len) è un int.
         - Verificare tassativamente che: `len >= 0` e che `len <= LIMITE_MASSIMO_RAGIONEVOLE` (es. 16 MB).
         - Se la validazione fallisce, registrare l'errore ed eseguire abort pulito senza incorrere in integer overflow o conversioni pericolose a size_t.
      4. Atomicità delle scritture:
         - Più thread mapper scrivono sullo STDOUT del Mapper verso il Reducer.
         - Più thread reducer scrivono sullo STDOUT del Reducer verso il Main.
         - Ogni operazione di emissione/scrittura di un messaggio completo (header + payload) deve essere protetta da un mutex C11 per evitare interleaving di byte tra messaggi diversi.
  
      ---
  
      ### 4. CONTRATTO SUI DATI: OPACITÀ E COPIE
      1. Token:
         - Stringa ASCII alfanumerica non vuota ([A-Za-z0-9]).
         - Nell'API pubblica è passata come stringa C terminata da '\0'.
         - Il terminatore '\0' NON fa parte del token logico e non è conteggiato in token_len.
         - Può essere manipolata con funzioni di stringa solo per ordinamento/confronto.
      2. Valori Intermedi e Risultati (processed_token e result):
         - DEVONO ESSERE TRATTATI COME SEQUENZE OPICHE DI BYTE.
         - Possono contenere byte nulli ('\0') o dati binari arbitrari.
         - È SEVERAMENTE VIETATO usare funzioni di stringa (strlen, strcmp, strcpy) o printf("%s") sui valori.
         - Se size == 0, il puntatore dati può essere NULL.
      3. Garanzia di Copia Immediata nelle Callback:
         - Le funzioni emit (`mr_emit_pair_t` e `mr_emit_result_t`) DEVONO copiare integralmente in memoria locale/buffer i dati ricevuti PRIMA di ritornare al chiamante. L'utente ha il
    diritto di liberare o sovrascrivere i buffer immediatamente dopo il ritorno da emit().
         - Le strutture `mr_file_line_t` passate al mapper sono valide solo per la durata dell'invocazione del mapper.
  
      ---
  
      ### 5. INPUT, ORDINAMENTO E DETERMINISMO DELL'OUTPUT
      1. Input:
         - Supportare singolo file regolare o directory.
         - Se directory: selezionare tutti i file regolari direttamente contenuti (senza ricorsione di default, oppure con ricorsione se abilitato l'addendum) ordinati in modo
    lessicografico per nome prima dell'apertura.
         - Gestire correttamente file vuoti, righe vuote (line_len = 0), file a singola riga e righe senza '\n' finale.
         - Assegnare line_number a partire da 1.
      2. Determinismo Output:
         - Il file di output binario finale generato dal Main deve essere rigorosamente deterministico e riproducibile a parità di parametri.
         - I record di output devono essere scritti ordinati lessicograficamente per token.
         - Se per uno stesso token ci sono più risultati emessi dal reducer, mantenerli ordinati in modo deterministico e documentato.
         - Formato record output:
           `[int token_len] [token bytes] [int result_len] [result bytes]` (senza '\0').
  
      ---
  
      ### 6. LOGGING CONCORRENTE
      1. File di log (default `mr.log` o impostato da `mr_attr_set_log_file()`).
      2. Formato: `[timestamp] [processo] [thread] [evento] messaggio`
      3. Eventi minimi obbligatori tracciati:
         - Creazione pipe
         - Creazione processi mapper e reducer
         - Avvio e join dei thread C11
         - Apertura/chiusura file input e output
         - Statistiche: righe inviate al mapper, coppie prodotte, token distinti raggruppati, risultati finali emessi
         - Errori di sistema
      4. Accesso concorrente multiprocesso e multithread protetto tramite semaforo POSIX o flock.
  
      ---
  
      ### 7. STRUTTURA DEL PROGETTO E DEL CODICE
      Organizza il workspace con la seguente struttura:
      - `include/mr.h`: Header pubblico con tutte le definizioni richieste dal PDF (incluso supporto all'addendum per hashing deterministico).
      - `src/`:
        - `mr_api.c`: Implementazione delle funzioni pubbliche (mr_attr_*, mr_create, mr_start, mr_destroy).
        - `mr_common.h` / `mr_common.c`: Primitive I/O (readn, writen), protocollo, logging thread-safe/process-safe.
        - `mr_queue.h` / `mr_queue.c`: Coda FIFO circolare bounded sincronizzata con mtx_t e cnd_t C11.
        - `mr_mapper.h` / `mr_mapper.c`: Logica del processo Mapper, reader thread, worker threads C11, sincronizzazione emit.
        - `mr_reducer.h` / `mr_reducer.c`: Logica del processo Reducer, reader thread, tabella di raggruppamento dinamica, worker threads C11, sincronizzazione emit.
      - `examples/`:
        - `word_count.c`: Esempio applicativo completo che conta le occorrenze delle parole con serializzazione di un int.
        - `dump_output.c`: Utility ausiliaria per leggere e stampare in forma testuale leggibile il file di output binario prodotto dal framework.
      - `tests/`:
        - Suite di test automatizzati che copre:
          * File singoli, file vuoti, righe vuote, ultima riga senza newline.
          * Directory con ordinamento lessicografico.
          * Valori binari contenenti byte nulli '\0' per verificare l'opacità dei dati.
          * Verifica del determinismo con esecuzioni multiple concorrenti.
          * Stress test con elevato numero di righe e token.
          * Gestione errori (parametri non validi, file inesistenti, ecc.).
      - `Makefile`:
        - Target `all` / `make`: compila `libmr.a` (in una cartella `lib/` o root) e gli esempi. Flag di compilazione: `-std=c11 -Wall -Wextra -Werror -pedantic -O2`.
        - Target `test` (`make test`): esegue l'intera batteria di test automatici verificando con assert e codici di ritorno il successo.
        - Target `clean` (`make clean`): pulizia completa di .o, .a, eseguibili, file di test e log temporanei.
      - `README.md`: Documentazione completa di architettura, istruzioni di build, test ed esecuzione.
  
      Implementa l'intero codice prestando la massima attenzione all'assenza di memory leak (verificabile con Valgrind), all'assenza di race condition (Helgrind/DRD compliant) e alla
    corretta gestione di ogni chiamata di sistema controllando sempre il valore di ritorno.
