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

- **File > Apri...** (Ctrl+O) apre un file video o audio. Al centro c'è l'anteprima; a destra formato del contenitore, durata, flussi (codec, risoluzione, fotogrammi al secondo, canali, bitrate...) e l'elenco dei fotogrammi chiave, cioè i punti in cui si potrà tagliare senza ricodifica. Un clic su un fotogramma chiave porta l'anteprima in quel punto.
- Si può anche passare il file da riga di comando o trascinarlo sull'icona di `EditorVideo.exe`: `EditorVideo.exe "C:\Video\vacanze.mp4"`.

### Anteprima

| Comando | Tasto |
|---|---|
| Riproduci / Pausa | Spazio (o il pulsante ▶) |
| Indietro / avanti di 5 secondi | ← / → |
| Fotogramma precedente / successivo | `,` / `.` (oppure Ctrl+← / Ctrl+→) |
| Torna all'inizio | Home |
| Vai a un punto | clic o trascinamento sulla linea del tempo |
| Segna IN / OUT | I / O |
| Vai a IN / OUT | Maiusc+I / Maiusc+O |
| Cancella IN e OUT | menu Segmento |
| Aggiungi il tratto IN-OUT all'elenco | A (menu Modifica) |
| Taglia via il tratto IN-OUT | X |
| Elimina il tratto sotto il cursore | Canc |
| Svuota l'elenco dei tratti | menu Modifica |
| Esporta (elenco dei tratti, o il solo IN-OUT) | Ctrl+E (menu File) |

### Segmento (IN e OUT)

- Sulla linea del tempo le tacche sono i fotogrammi chiave; il segmento fra IN (verde) e OUT (rosso) è evidenziato, e sotto c'è la sua durata.
- **IN** si aggancia da solo al fotogramma chiave nello stesso punto o subito prima: senza ricodifica un video può iniziare solo da lì. Se IN viene spostato, sotto la durata compare da dove a dove.
- **OUT** può stare su qualsiasi fotogramma dopo IN e comprende il fotogramma che si vede.
- Senza IN il segmento parte dall'inizio del file, senza OUT arriva fino alla fine. Se si sposta IN dopo OUT, OUT viene tolto.
- Si segna sempre il fotogramma visibile: conviene mettere in pausa e avvicinarsi con `,` e `.`.

### Più tratti (menu Modifica)

- Si possono tenere più pezzi dello stesso file: segna IN e OUT e premi **A** per aggiungere il tratto all'elenco; IN e OUT si liberano per il tratto successivo. Sulla linea del tempo i tratti tenuti sono in verde e il resto è scurito; sotto c'è il numero di tratti e la durata finale.
- Oppure al contrario: segna IN e OUT del pezzo da togliere (es. la pubblicità) e premi **X**. Se l'elenco è vuoto si parte dal file intero.
- I tratti sono sempre in ordine di tempo; quelli che si toccano o si sovrappongono vengono uniti. **Canc** elimina il tratto sotto il cursore.
- Come per IN, ogni tratto inizia su un fotogramma chiave: togliendo un pezzo, la parte dopo riparte dal fotogramma chiave più vicino prima della fine del pezzo tolto, quindi possono restare alcuni fotogrammi del pezzo tolto (e se dentro il pezzo non c'è un fotogramma chiave non si può togliere: un messaggio lo dice).
- **Ctrl+E** esporta tutti i tratti uniti in un unico file; se l'elenco è vuoto esporta il solo IN-OUT.

### Esportazione

- **File > Esporta segmento...** (Ctrl+E), disponibile quando IN e OUT sono segnati, salva il segmento in un nuovo file: propone `<nome>_taglio.<estensione>` nella cartella dell'originale.
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
- A ogni push GitHub Actions compila la soluzione in Release, lancia i test, avvia l'applicazione con `--smoke-test`, prova la riproduzione dei file dimostrativi e l'esportazione di un segmento.
