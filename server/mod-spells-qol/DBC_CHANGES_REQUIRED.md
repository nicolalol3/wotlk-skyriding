# MODIFICHE DBC NECESSARIE PER MOD-SPELLS-QOL

Questo modulo richiede modifiche manuali ai file DBC (AreaTable.dbc) per funzionare correttamente, specialmente per quanto riguarda il volo globale.

### Modifiche effettuate (AreaTable):
Per permettere il volo in tutte le zone del gioco (Azeroth, Northrend, ecc.), è stato necessario patchare la colonna `Flags` della tabella `AreaTable`.

1. **Abilitazione Volo**: È stato aggiunto il bit `0x400` (1024) a tutti gli ID area che permettono l'uso di cavalcature (Flags != 0). Questo rende la zona "Flyable" per il client e per il server.
2. **Rimozione Blocchi**: È stato rimosso il bit `0x20000000` (536870912), che solitamente attiva l'aura di disarcionamento automatico (usata originariamente a Dalaran).

### Avvertenze per il Futuro:
In caso di installazione di mod che cambiano le mappe (es. nuove zone o modifiche a Dalaran), queste patch ai DBC andranno **ripetute**. 
In particolare, se gli ID di Dalaran dovessero cambiare o essere sovrascritti, il modulo C++ (`ridingchanges.cpp`) potrebbe non riconoscere più correttamente le zone del "balcone" (Krasus' Landing) e della "città", causando il blocco del volo dove non dovrebbe o viceversa.

**ID Attuali Krasus' Landing:** 4564, 4598.
