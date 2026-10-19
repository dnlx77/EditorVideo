# EditorVideo

Editor video per Windows: taglio e unione di video e audio **senza ricodifica** (l'output resta nel formato di partenza).

Il piano di sviluppo per step è nel progetto Claude "Editor video".

## Requisiti

- Visual Studio 2026 con il carico di lavoro **Sviluppo di applicazioni desktop con C++** e il componente **Gestione pacchetti vcpkg** (incluso di default in quel carico di lavoro).
- Git.

Qt 6, FFmpeg e Google Test **non** vanno installati a mano: li scarica e compila vcpkg dal file `vcpkg.json`.

## Primo avvio

1. Clona il repository dove preferisci:
   ```
   git clone https://github.com/dnlx77/EditorVideo
   ```
2. Apri `EditorVideo.slnx` in Visual Studio 2026.
3. Scegli la configurazione `Debug | x64` (o `Release | x64`) e compila la soluzione (Ctrl+Maiusc+B).
   La **prima compilazione è lunga** (anche 30-60 minuti): vcpkg compila Qt e FFmpeg nella cartella `vcpkg_installed`. Le volte successive è questione di secondi.
4. Imposta `EditorApp` come progetto di avvio e premi F5.

Se la compilazione dice "vcpkg non trovato", installa il componente vcpkg dal Visual Studio Installer oppure clona vcpkg (`git clone https://github.com/microsoft/vcpkg C:\vcpkg` e poi `C:\vcpkg\bootstrap-vcpkg.bat`) e imposta la variabile d'ambiente `VCPKG_ROOT=C:\vcpkg`, poi riapri Visual Studio.

## Struttura

| Progetto | Tipo | Contenuto |
|---|---|---|
| `src/EditorCore` | libreria statica | logica senza interfaccia: lettura file, taglio, unione (FFmpeg) |
| `src/EditorApp` | applicazione Qt 6 | interfaccia grafica, eseguibile `EditorVideo.exe` |
| `tests/EditorTests` | console (Google Test) | test automatici di `EditorCore` |

Gli eseguibili finiscono in `bin\x64\<Configurazione>\`, insieme alle DLL di Qt e FFmpeg copiate da `vcpkg_installed`.

## Uso

La finestra è divisa così:

- **a sinistra**: i **file del progetto** (ognuno col suo colore), e sotto le informazioni sul file scelto e i suoi fotogrammi chiave;
- **al centro**: l'anteprima **Sorgente**, cioè il file scelto, dove si segnano IN e OUT;
- **a destra**: l'anteprima **Montaggio**, il risultato finale, con i pezzi uno dopo l'altro;
- **in basso**: la **timeline del montaggio**, con un blocco colorato per ogni pezzo (il colore del file da cui viene), largo quanto dura, e sotto la **corsia audio** con la forma d'onda di ogni pezzo.

**File > Apri file video...** (Ctrl+O) apre un file in un progetto nuovo; **File > Aggiungi file...** (Ctrl+Maiusc+O, anche più file insieme) ne aggiunge altri. Vengono analizzati uno alla volta in background. Si possono anche passare i file da riga di comando: `EditorVideo.exe "C:\Video\a.mp4" "C:\Video\b.mp4"`.

### Comandi

| Comando | Tasto |
|---|---|
| Riproduci / Pausa (l'anteprima usata per ultima, segnata con ▶ se è la Sorgente) | Spazio |
| Indietro / avanti di 5 secondi | ← / → |
| Torna all'inizio | Home |
| Fotogramma precedente / successivo (Sorgente) | `,` / `.` (oppure Ctrl+← / Ctrl+→) |
| Segna IN / OUT (Sorgente; con il Montaggio attivo: inizio e fine del tratto audio) | I / O |
| Vai a IN / OUT | Maiusc+I / Maiusc+O |
| Cancella IN e OUT | menu Segmento |
| **Aggiungi IN-OUT al montaggio** (senza IN e OUT: il file intero) | A |
| Togli IN-OUT dal montaggio | X |
| Dividi il pezzo sotto il cursore del montaggio | S |
| Elimina il pezzo selezionato (o il file, se il fuoco è sull'elenco dei file) | Canc |
| Svuota il montaggio | menu Montaggio |
| Togli l'audio dal tratto selezionato | M |
| Rimetti l'audio nel tratto selezionato | Maiusc+M |
| Sostituisci l'audio / torna all'audio originale | menu Audio |
| Esporta il montaggio (o, se è vuoto, il solo IN-OUT) | Ctrl+E |
| Salva progetto / Salva progetto con nome | Ctrl+S / Ctrl+Maiusc+S |
| Apri progetto | Ctrl+P |

### Sorgente: IN e OUT

- Sulla linea del tempo le tacche sono i fotogrammi chiave; il segmento fra IN (verde) e OUT (rosso) è evidenziato, e sotto c'è la sua durata. In verde chiaro le parti del file già usate nel montaggio.
- **IN** si aggancia da solo al fotogramma chiave nello stesso punto o subito prima: senza ricodifica un video può iniziare solo da lì. Se IN viene spostato, sotto la durata compare da dove a dove.
- **OUT** può stare su qualsiasi fotogramma dopo IN e comprende il fotogramma che si vede.
- Si segna sempre il fotogramma visibile: conviene mettere in pausa e avvicinarsi con `,` e `.`.

### Montaggio

- Scegli un file, segna I e O nella Sorgente e premi **A**: il pezzo va nella timeline del montaggio, dopo il pezzo selezionato (o in fondo). IN e OUT si liberano per il pezzo successivo. Lo stesso file si può usare più volte, in qualsiasi ordine.
- Nella timeline: **clic** per spostare lì l'anteprima Montaggio, **trascina** un blocco per cambiarne il posto, **doppio clic** per riaprirlo nella Sorgente con i suoi IN e OUT, **Canc** per toglierlo, **S** per dividerlo al cursore.
- **S** divide al fotogramma chiave più vicino prima del cursore (la seconda parte deve iniziare da un fotogramma chiave): non si perde né si ripete nessun fotogramma.
- **X** toglie dal montaggio il pezzo fra IN e OUT segnati nella Sorgente (es. la pubblicità), da tutti i pezzi di quel file che lo contengono. La parte dopo riparte dal fotogramma chiave più vicino prima della fine del pezzo tolto, quindi può restare qualche fotogramma; se dentro non c'è un fotogramma chiave, o il pezzo non è nel montaggio, un messaggio lo dice.
- L'anteprima Montaggio passa da un pezzo all'altro aprendo il file giusto: a ogni passaggio può esserci una brevissima pausa (solo nell'anteprima, il file esportato no).
- Senza ricodifica si possono unire solo file con gli **stessi codec e parametri** (risoluzione, formato pixel, fotogrammi al secondo, profilo, parametri di codifica; per l'audio frequenza e canali): di solito file dello stesso telefono o della stessa videocamera con le stesse impostazioni. Se qualcosa non coincide l'esportazione viene rifiutata, con un messaggio che dice quale file e cosa è diverso (es. `Clip 3 (nome.mp4): risoluzione 1280x720 invece di 1920x1080`). Contenitori diversi vanno bene, se i codec coincidono.
### Audio

- Sotto i pezzi, la **corsia audio** mostra la forma d'onda di ognuno (calcolata in background la prima volta che si aggiunge un file; finché non è pronta c'è scritto "forma d'onda...").
- **Togliere l'audio da un tratto**: si seleziona il tratto **trascinando sulla corsia audio** (oppure, con il Montaggio attivo, con **I** e **O** alla posizione del cursore) e si preme **M**. Il tratto diventa barrato in rosso: lì l'anteprima Montaggio e il file esportato sono in silenzio, il video resta com'è. **Maiusc+M** rimette l'audio nel tratto selezionato. Il silenzio appartiene al pezzo: se il pezzo viene spostato, lo segue.
- **Sostituire l'audio**: **Audio > Sostituisci l'audio...** sceglie un file (MP3, M4A, AAC, WAV... o anche un video, di cui si usa l'audio) che diventa l'audio di tutto il montaggio, al posto di quello originale. Sulla corsia audio compare come un blocco giallo: trascinandone la **maniglia** (la striscia col nome) lo si sposta avanti o indietro rispetto al video. Prima dell'inizio della traccia c'è silenzio; alla fine del montaggio la traccia viene tagliata. I tratti barrati valgono anche per la traccia sostitutiva. **Audio > Usa l'audio originale** torna all'audio dei file.
- Anche l'audio sostitutivo viene copiato senza ricodifica, quindi deve poter stare nel contenitore del file esportato: per un MP4 vanno bene AAC/M4A e MP3, non un WAV. Se non va bene, un messaggio lo dice subito (con un MKV vanno bene quasi tutti i formati).

### Progetto

- **File > Salva progetto** (Ctrl+S) salva il lavoro in un file `.evproj`: i file usati, i pezzi del montaggio, i tratti senza audio, l'audio sostitutivo con il suo spostamento, IN e OUT della Sorgente e dove erano le anteprime. Il video e l'audio originali non vengono mai toccati: il progetto li indica soltanto. **Salva progetto con nome...** (Ctrl+Maiusc+S) ne crea una copia con un altro nome.
- **File > Apri progetto...** (Ctrl+P; Ctrl+O resta per aprire un file video) e **File > Progetti recenti** (gli ultimi 5) lo riaprono; anche `EditorVideo.exe montaggio.evproj`.
- Il progetto ricorda per ogni file sia il percorso completo sia quello relativo alla sua cartella: se si sposta tutta la cartella (progetto e video insieme, anche su un altro disco) i file si ritrovano. Se un file non c'è più, un messaggio elenca quelli mancanti: **Trova...** permette di indicare dove sono ora (gli altri mancanti vengono cercati nella stessa cartella), **Continua senza** toglie i loro pezzi dal montaggio. Se un file è cambiato (dura diversamente da quando è stato salvato il progetto) compare un avviso.
- Nel titolo della finestra c'è il nome del progetto, con un asterisco se ci sono modifiche non salvate; chiudendo il programma o aprendo un altro progetto viene chiesto se salvarle.
- Il file è JSON leggibile (UTF-8). I tempi sono numeri interi di "tick" da 1/705 600 000 di secondo, un'unità che divide esattamente le basi dei tempi di video e audio (1/90000, 1/1000, 1/48000, 1/44100, 1001/30000...): riaprendo un progetto ogni tempo torna identico. Un progetto salvato da una versione più recente del programma viene rifiutato con un messaggio, così come un file rovinato.

### Esportazione

- **File > Esporta...** (Ctrl+E) salva il montaggio in un nuovo file, nel contenitore del primo pezzo: propone `<nome>_montaggio.<estensione>` nella cartella dell'originale. Se il montaggio è vuoto esporta il solo IN-OUT della Sorgente (`<nome>_taglio.<estensione>`).
- **Senza ricodifica**: video e audio vengono copiati così come sono (stessi codec, stessa qualità, nessuna perdita) nello stesso contenitore dell'originale (MP4 resta MP4, MKV resta MKV...). Per questo è anche molto veloce.
- Il video parte dal fotogramma chiave di IN, l'audio insieme a lui; la durata finale è quella mostrata sotto la linea del tempo (con al massimo qualche centesimo di differenza). Si copiano video e audio; sottotitoli e altri flussi per ora no.
- Con più tratti, nel file creato si susseguono senza buchi: a ogni giunzione video e audio ripartono insieme. L'audio PCM (es. WAV) viene tagliato al campione esatto; l'audio compresso (AAC, MP3...) a pacchetti, quindi a ogni giunzione può mancare al massimo un pacchetto (circa 20 ms).
- L'originale non viene mai modificato. Se si annulla o c'è un errore, non resta un file a metà.

- Il video viene decodificato dalla **scheda video (D3D11VA)** quando il codec lo permette (H.264, HEVC, VP9, AV1, MPEG-2... secondo la GPU), altrimenti dal **processore**: il passaggio è automatico, anche se la GPU dà errore a metà. In basso a destra c'è scritto quale si sta usando (`Decodifica: GPU (D3D11VA)` / `Decodifica: software`).
- L'audio è il riferimento per la sincronizzazione: ogni fotogramma viene mostrato quando l'audio arriva al suo tempo. Se il file non ha audio (o non c'è una scheda audio) si usa l'orologio di sistema.
- L'audio esce dalla scheda audio predefinita di Windows (waveOut), così non serve il modulo Qt Multimedia.

## Test

- Esegui `EditorTests` (progetto di avvio o da `bin\x64\Debug\EditorTests.exe`), oppure usa Esplora test di Visual Studio.
- I test generano al volo piccoli video di prova (MKV, MP4, WAV) con FFmpeg nella cartella temporanea: nel repository non ci sono file binari. Con la variabile d'ambiente `EDITORVIDEO_KEEP_SAMPLES=1` i file dei test di analisi restano in `%TEMP%\EditorVideoTests`.
- Per provare l'anteprima senza file propri: `EditorTests.exe --gtest_also_run_disabled_tests --gtest_filter=*GeneraVideoDimostrativi*` crea in `%TEMP%\EditorVideoDemo` due video da 10 secondi (uno MPEG-2, che di solito la GPU decodifica, e uno MPEG-4 Part 2, sempre in software) e un file solo audio. L'audio è muto.
- `EditorVideo.exe --smoke-test` verifica che l'applicazione parta con tutte le DLL; `EditorVideo.exe --smoke-test file.mkv` riproduce anche il file per 2 secondi, controlla che la posizione avanzi col tempo giusto, segna due tratti e li esporta uniti dalla finestra (controllando anche che la finestra di avanzamento si chiuda) (con `EDITORVIDEO_SMOKE_SCREENSHOT=immagine.png` salva anche un'immagine della finestra). Con `EDITORVIDEO_NO_AUDIO=1` si simula un PC senza scheda audio.
- `EditorVideo.exe --export-test origine destinazione IN OUT [IN OUT ...]` (tempi in secondi) esporta uno o più tratti uniti come l'interfaccia, con ogni IN agganciato al fotogramma chiave, e controlla il file creato (codici di uscita: 0 tutto giusto, 2 errore, 3 controllo non riuscito).
- `EditorVideo.exe --join-test destinazione file1 file2 [...]` unisce i file interi come l'interfaccia, dopo il controllo di compatibilità, e controlla il file creato (codici di uscita: 0 tutto giusto, 4 file non compatibili, 2 errore, 3 controllo non riuscito).
- `EditorVideo.exe --sequence-test destinazione file IN OUT [file IN OUT ...]` costruisce un montaggio con i pezzi in quell'ordine, lo esporta e controlla durata e ordine (all'inizio di ogni pezzo lo stesso fotogramma dell'originale).
- `EditorVideo.exe --audio-test destinazione file [mute DA A]... [replace audio OFFSET] [omit]` mette il file intero nel montaggio, toglie l'audio dai tratti indicati e/o lo sostituisce, esporta e controlla durata, codec, che l'audio sia continuo (nessun buco nuovo fra un blocco e l'altro) e che nei tratti ci sia silenzio. Codice 4 se l'audio sostitutivo non può stare nel contenitore.
- `EditorVideo.exe --project-test file [file ...]` prova i progetti con file veri, in una cartella temporanea: salva un montaggio (con audio tolto e sostituito), lo riapre e lo risalva (deve essere identico), sposta la cartella (i file devono ritrovarsi), esporta il progetto riaperto, poi toglie un file (deve risultare mancante) e prova un progetto di versione sconosciuta e uno rovinato (devono essere rifiutati).
- `--smoke-test` con uno o più file costruisce un montaggio dall'interfaccia, prova l'anteprima Montaggio a cavallo di una giunzione e lo esporta. Con la variabile d'ambiente `QT_QPA_PLATFORM=minimal` le prove girano senza aprire finestre.
- A ogni push GitHub Actions compila la soluzione in Release, lancia i test, avvia l'applicazione con `--smoke-test`, prova la riproduzione dei file dimostrativi, l'esportazione di uno e più tratti e l'unione di più file (anche il rifiuto di file non compatibili).
