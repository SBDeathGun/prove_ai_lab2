# Regole del Progetto e Gestione dello Stato di Avanzamento

## Session State & Handover Policy (Tracciamento Stato e Ripresa Sessioni)

Per evitare la perdita di contesto causata da limiti di token, lunghe conversazioni, compattazione della cronologia o interruzioni tra sessioni diverse, l'agente deve attenersi alle seguenti linee guida:

### 1. Documento di Handover Continuo (`PROGRESS.md`)
- Durante l'esecuzione di task complessi o che richiedono più passaggi, mantieni sempre aggiornato un file di tracciamento nella radice del progetto: `PROGRESS.md`.
- Aggiorna il file periodicamente dopo ogni modifica significativa o prima di completare risposte articolate, in modo che lo stato corrente sia sempre salvato su disco.

### 2. Struttura del file `PROGRESS.md`
Il documento deve includere:
- **Obiettivo generale**: Cosa si vuole realizzare e il contesto del task.
- **Attività completate**: Lista dei file creati/modificati e funzionalità implementate.
- **Stato corrente (Cosa stavo facendo)**: Dettaglio esatto di cosa era in corso di svolgimento nel momento corrente, spiegando il flusso logico interrotto.
- **Prossimi passi**: I passaggi immediati da eseguire per completare o continuare il lavoro.
- **Note tecniche e decisioni**: Scelte architetturali, comandi eseguiti o eventuali problemi/errori riscontrati.

### 3. Ripresa del Lavoro
- All'inizio di ogni nuova sessione di chat o quando viene richiesto di continuare il lavoro, leggi prioritariamente `PROGRESS.md` per riallineare il contesto e riprendere senza esitazioni.
