# EditorVideo

Editor video per Windows: taglio e unione di video e audio. Si copia senza ricodificare dove si può; si ricodifica solo dove serve (tagli precisi, file con un formato diverso da quello del progetto, spazi neri), e l'output resta nel formato del progetto.

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

### Trascinare i file

- Si possono **trascinare file da Esplora file** sulla finestra: ovunque li si lasci vengono aggiunti all'elenco dei file del progetto (quelli che ci sono già non vengono ripetuti).
- Lasciati sulla **Sorgente**, l'ultimo si apre anche lì. Lasciati sulla **linea del tempo del montaggio**, vanno anche nel montaggio, interi, nel punto in cui si lasciano (una linea mostra dove) e nell'ordine in cui sono stati trascinati.
- Una **cartella** aggiunge i video e gli audio che contiene (non quelli delle sottocartelle). Un **progetto** (.evproj) si apre, chiedendo prima se salvare quello aperto. I file che non sono video o audio leggibili vengono elencati in un messaggio; gli altri si aggiungono lo stesso.
- Ogni trascinamento si annulla con un solo **Ctrl+Z** ("Annulla Aggiungi file").

### Pannelli

- File del progetto, Sorgente, Montaggio e la linea del tempo del montaggio sono **pannelli**: si ridimensionano trascinando i bordi, si spostano trascinando il titolo (anche uno accanto all'altro o uno sopra l'altro, o come schede) e si **staccano** come finestre a sé con il pulsante sul titolo o un doppio clic sul titolo, per esempio per mettere il Montaggio su un secondo monitor. Doppio clic sul titolo di una finestra staccata per riagganciarla.
- La linea del tempo del montaggio si aggancia sempre in basso, a tutta larghezza. Staccata, si riaggancia lasciandola in un punto qualsiasi della metà bassa della finestra, con un doppio clic sul suo titolo o con **Finestra > Riaggancia la linea del tempo**.
- Dove un bordo verticale e uno orizzontale fra i pannelli si incrociano il cursore diventa una croce: trascinando l'incrocio si cambiano insieme larghezza e altezza. I pannelli staccati si ridimensionano anche dagli angoli della loro finestra.
- I pannelli possono diventare piccoli (circa 120x100): quando lo spazio è poco nascondono le righe secondarie (nella Sorgente prima le scritte sotto, poi la linea del tempo; nei file le schede Informazioni e Fotogrammi chiave).
- **Doppio clic sull'immagine** della Sorgente o del Montaggio: quell'anteprima a schermo intero; **Esc** o un altro doppio clic per uscire. Spazio, frecce e gli altri tasti funzionano anche a schermo intero e nei pannelli staccati.
- Sotto la Sorgente e il Montaggio ci sono i **pulsanti di riproduzione**: inizio, fotogramma chiave (Sorgente) o taglio (Montaggio) precedente, fotogramma indietro, riproduci/pausa, **stop** (ferma e torna dove era partita la riproduzione), fotogramma avanti, segno successivo, fine; nella Sorgente anche IN, OUT, vai a IN / OUT e "+ Montaggio". A destra la **velocità** (0,25x - 4x; fino a 2x l'audio si sente con la stessa intonazione, oltre e all'indietro è muto), la **ripetizione** (Sorgente: fra IN e OUT o tutto il file; Montaggio: tutto il montaggio) e il **volume** dell'anteprima (non cambia il file esportato). Con poco spazio si nascondono prima i pulsanti meno importanti.
- Sopra le linee del tempo della Sorgente e del Montaggio c'è un **righello con i tempi** (ore:minuti:secondi:fotogrammi), con tacche più fitte quando si ingrandisce; un clic o un trascinamento sul righello sposta il cursore.
- Nel righello una **barretta celeste** mostra il tratto del video già letto e tenuto in memoria intorno al cursore: fino a circa 10 secondi avanti e 5 indietro. Lì dentro i salti e i passi indietro sono immediati, senza rileggere il file.
- **Qualità dell'anteprima**, **per ogni file** (menu a tendina nella barra dei comandi: quello della Sorgente cambia il file nella Sorgente, quello del Montaggio il file del pezzo mostrato; es. proxy per un 4K e originale per un 1080p): **Originale** mostra il file così com'è (all'inizio tutti i file); **1/2, 1/4, 1/8** usano i **proxy**, copie leggere dei file a quella frazione della risoluzione con un fotogramma chiave ogni 10 fotogrammi. Con i video pesanti (es. 4K con i fotogrammi chiave a secondi di distanza) i salti diventano immediati. I proxy si creano in sottofondo, uno alla volta (prima quello del file nella Sorgente): con una scheda NVIDIA la lettura e la riduzione del video le fa la scheda video (NVDEC), altrimenti il processore, e la codifica usa l'encoder della scheda video; a fine creazione la barra di stato dice come è stato fatto; l'avanzamento si vede nell'elenco dei file e l'anteprima passa al proxy appena è pronto, senza perdere la posizione. Hanno gli stessi tempi dei fotogrammi degli originali, quindi IN, OUT e tagli non cambiano; **l'esportazione usa sempre gli originali**. Stanno accanto al progetto, nella cartella `<nome progetto>.proxy` (per un progetto mai salvato nella cartella di lavoro, in `proxy`, e al salvataggio si spostano accanto al progetto); se un originale cambia, il suo proxy si rifà. Se il proxy della qualità scelta non è ancora pronto ma ce n'è uno di un'altra qualità (es. 1/2 pronto mentre si crea quello a 1/4), l'anteprima usa quello invece dell'originale. Nella timeline del montaggio i pezzi letti da un proxy hanno l'etichetta celeste **proxy 1/N**, e sotto l'anteprima la scritta della decodifica dice "dal proxy 1/N". Cambiando la qualità di un file si interrompe solo il proxy di quel file: quelli degli altri file in creazione vanno avanti. **File > Elimina i proxy del progetto** li cancella e riporta tutti i file agli originali. La qualità di ogni file si salva nel progetto.
- La disposizione viene salvata chiudendo il programma e ritrovata alla riapertura (se un monitor non c'è più, la finestra torna su quello principale). Il menu **Finestra** mostra o nasconde ogni pannello e ha **Ripristina disposizione** per tornare a quella iniziale.

### Comandi

| Comando | Tasto |
|---|---|
| Riproduci / Pausa (l'anteprima usata per ultima, segnata con ▶ nel titolo del suo pannello) | Spazio |
| Spola: avanti / all'indietro (premuti più volte: 2x, 4x) / pausa | L / J / K |
| Fotogramma precedente / successivo | ← / → (oppure `,` / `.`) |
| Sorgente: fotogramma chiave precedente / successivo · Montaggio: inizio del pezzo precedente / successivo | Maiusc+← / Maiusc+→ |
| Indietro / avanti di 5 secondi | Ctrl+← / Ctrl+→ |
| Inizio / fine | Home / Fine |
| Ingrandisci / riduci la linea del tempo dell'anteprima attiva | + / - (oppure Ctrl+rotellina sulla linea del tempo) |
| Segna IN / OUT (Sorgente) | I / O |
| Vai a IN / OUT | Maiusc+I / Maiusc+O |
| Cancella IN e OUT | menu Segmento |
| **Aggiungi IN-OUT al montaggio** (senza IN e OUT: il file intero) | A |
| Togli IN-OUT dal montaggio | X |
| Dividi il pezzo sotto il cursore del montaggio | S |
| Elimina il pezzo selezionato (o il file, se il fuoco è sull'elenco dei file) | Canc |
| Svuota il montaggio | menu Montaggio |
| Sostituisci l'audio / torna all'audio originale | menu Audio |
| Esporta il montaggio (o, se è vuoto, il solo IN-OUT) | Ctrl+E |
| Salva progetto / Salva progetto con nome | Ctrl+S / Ctrl+Maiusc+S |
| Apri progetto | Ctrl+P |
| Annulla / Ripeti (qualsiasi modifica a file, montaggio e audio) | Ctrl+Z / Ctrl+Y (oppure Ctrl+Maiusc+Z) |
| Questo elenco | F1 (menu Aiuto > Scorciatoie da tastiera) |

### Sorgente: IN e OUT

- Sulla linea del tempo le tacche sono i fotogrammi chiave; il segmento fra IN (verde) e OUT (rosso) è evidenziato, e sotto c'è la sua durata. In verde chiaro le parti del file già usate nel montaggio.
- **IN** e **OUT** restano esattamente sul fotogramma scelto (**taglio preciso**): all'esportazione la parte fra IN e il primo fotogramma chiave dopo si ricodifica e il resto si copia (vedi Esportazione). Se IN cade fra due fotogrammi chiave, sotto la durata un avviso lo dice.
- **OUT** può stare su qualsiasi fotogramma dopo IN e comprende il fotogramma che si vede.
- Si segna sempre il fotogramma visibile: conviene mettere in pausa e avvicinarsi con `,` e `.`.

### Montaggio

- Scegli un file, segna I e O nella Sorgente e premi **A**: il pezzo va nella timeline del montaggio, dopo il pezzo selezionato (o in fondo). IN e OUT si liberano per il pezzo successivo. Lo stesso file si può usare più volte, in qualsiasi ordine.
- Nella timeline: **clic** per spostare lì l'anteprima Montaggio, **trascina** un blocco per spostarlo avanti o indietro, **doppio clic** per riaprirlo nella Sorgente con i suoi IN e OUT, **Canc** per toglierlo (quelli dopo scorrono indietro), **S** per dividerlo al cursore.
- **Spazi vuoti**: trascinando un blocco si sposta solo lui, gli altri restano dove sono, e fra i pezzi resta uno spazio vuoto (riquadro tratteggiato, con la durata). Un blocco non si sovrappone mai agli altri: si ferma contro i vicini, e vicino ai bordi degli altri blocchi si aggancia da solo. Trascinato sopra un altro blocco va nello spazio libero più vicino (anche dall'altra parte). Dopo l'ultimo pezzo la timeline lascia sempre un po' di posto per spostarlo più avanti.
- **Bordi dei pezzi**: vicino al bordo sinistro o destro di un blocco il cursore diventa ⟷; trascinando si sposta l'**inizio** o la **fine** del pezzo nel file, a passi di un fotogramma (fino all'inizio o alla fine del file, mai sotto un fotogramma di durata). Gli altri pezzi restano dove sono: allungando ci si ferma contro il vicino, accorciando resta uno spazio vuoto. **Con Alt premuto** i pezzi dopo scorrono insieme (il pezzo resta dove inizia). Il bordo si aggancia al cursore e ai bordi degli altri blocchi; mentre si trascina il Montaggio mostra il fotogramma del nuovo bordo e un suggerimento dice il nuovo tempo e di quanto è cambiato. I tratti senza audio che finiscono fuori dal pezzo si tagliano. Si annulla con Ctrl+Z.
- Uno spazio vuoto si **riempie** con **A** (clic sullo spazio per selezionarlo, poi A) o lasciandoci dei file trascinati: il pezzo parte dall'inizio dello spazio e gli altri non si muovono; se non ci sta, quelli dopo vanno avanti quanto serve. **Clic** su uno spazio e **Canc** lo chiude (i pezzi dopo scorrono indietro). Spostamenti e chiusure si annullano con Ctrl+Z.
- Nell'anteprima Montaggio gli spazi vuoti sono **neri e in silenzio**, e il tempo continua a scorrere; nel file esportato anche (vedi Esportazione).
- **S** divide esattamente al cursore: non si perde né si ripete nessun fotogramma (la seconda parte, se non inizia su un fotogramma chiave, avrà l'inizio ricodificato all'esportazione).
- **X** toglie dal montaggio il pezzo fra IN e OUT segnati nella Sorgente (es. la pubblicità), da tutti i pezzi di quel file che lo contengono. La parte dopo riparte esattamente dalla fine del pezzo tolto; se il pezzo non è nel montaggio un messaggio lo dice.
- L'anteprima Montaggio passa da un pezzo all'altro aprendo il file giusto: a ogni passaggio può esserci una brevissima pausa (solo nell'anteprima, il file esportato no).
- **Formato del progetto**: il primo pezzo messo nel montaggio decide il formato del file esportato (codec, risoluzione, fotogrammi al secondo, formato dei pixel, audio). **Montaggio > Impostazioni del progetto...** lo mostra e permette di prenderlo da un altro file del progetto; si salva con il progetto (i progetti di prima lo ricavano dal primo pezzo). Per ora codec, risoluzione e fotogrammi al secondo si prendono da un file del progetto.
- Ogni pezzo è confrontato con il formato del progetto. Un segno sul blocco lo dice (tenendo il mouse sopra c'è la spiegazione): nessun segno = si copia così com'è; **P** = stesso codec, misure e fotogrammi al secondo ma parametri di codifica diversi (es. un altro numero di fotogrammi di riferimento, come in alcuni video di ShadowPlay): si copia lo stesso, senza ricodifica; **R** = diverso (risoluzione, fotogrammi al secondo, codec, audio...): all'esportazione si ricodifica nel formato del progetto (vedi sotto).
- Si copiano senza ricodifica i pezzi con gli **stessi codec e parametri** del formato del progetto (risoluzione, formato pixel, fotogrammi al secondo, profilo, parametri di codifica; per l'audio frequenza e canali): di solito file dello stesso telefono o della stessa videocamera con le stesse impostazioni. Se qualcosa non coincide l'esportazione **non viene rifiutata**: il pezzo diverso (segno R) si ricodifica nel formato del progetto. Contenitori diversi vanno bene, se i codec coincidono.
### Audio

- Sotto i pezzi, la **corsia audio** mostra la forma d'onda di ognuno (calcolata in background la prima volta che si aggiunge un file; finché non è pronta c'è scritto "forma d'onda...").
- **Sostituire l'audio**: **Audio > Sostituisci l'audio...** sceglie un file (MP3, M4A, AAC, WAV... o anche un video, di cui si usa l'audio) che diventa l'audio di tutto il montaggio, al posto di quello originale: va sulla traccia **A2** e A1 (l'audio originale) diventa muta. Trascinando la clip la si sposta avanti o indietro rispetto al video. Prima dell'inizio della traccia c'è silenzio; alla fine del montaggio la traccia viene tagliata. I tratti barrati valgono anche per la traccia sostitutiva. **Audio > Usa l'audio originale** torna all'audio dei file.
- **Tracce audio**: sotto la corsia video ci sono **A1** (l'audio dei pezzi video) e le tracce aggiunte **A2, A3...**, ognuna con a sinistra il nome, i pulsanti **M** (muto) e **S** (solo) e il **livello** in dB (trascinalo; doppio clic = 0 dB). I pulsanti **+** e **−** in alto a sinistra della linea del tempo (o **Audio > Aggiungi traccia audio / Togli traccia audio**) aggiungono una traccia e tolgono quella scelta (clic sul suo nome o su una sua clip; altrimenti l'ultima).
  - Le **clip audio** entrano trascinando file audio (da Esplora file o dall'elenco dei file del progetto) su una traccia aggiunta, oppure con **Audio > Aggiungi file audio al cursore...**. Si spostano trascinandole (in verticale cambiano traccia), si accorciano dai bordi come i pezzi video (con Alt quelle dopo scorrono), **S** divide la clip selezionata al cursore e **Canc** la toglie.
  - **Togliere un tratto**: trascinando sullo sfondo di una corsia aggiunta (A2, A3...) si seleziona un tratto; **Canc** lo toglie dalle clip di quella traccia. Su A1 il muto si fa con la linea del livello o con il pulsante M della traccia.
  - **Un file solo audio** (MP3, WAV...) non va mai nella traccia video del montaggio: con **A** (il tratto IN-OUT della Sorgente) o trascinandolo sulla linea del tempo finisce su una traccia audio, quella scelta o la prima (se non ce ne sono si aggiunge A2), dal cursore del montaggio.
  - **La traccia scelta** (clic sul suo nome, su una sua clip o su un tratto; per il video, un pezzo) è evidenziata: nome con barra piena e contorno attorno alla corsia.
  - **L'anteprima del Montaggio suona il mix**: A1 e tutte le tracce aggiunte si sommano a 48 kHz stereo, ognuna con il suo livello, muto e solo e con i tratti silenziati; un limitatore dolce evita la saturazione. Il livello, M e S si sentono subito anche mentre suona. L'orologio è quello dell'audio e non si interrompe ai cambi di pezzo (la musica sotto continua senza pause; il video si adatta). L'audio si sente da 0,25x a 2x in avanti; più veloce o all'indietro si va solo con il video. Senza scheda audio l'anteprima va a tempo reale.
  - **Selezione di più oggetti**: **Ctrl+clic** su un pezzo video, sull'audio di un pezzo (corsia A1) o su una clip audio lo aggiunge alla selezione (o lo toglie); sono tutti evidenziati. Un clic senza Ctrl riparte da un oggetto solo.
  - **Gruppi**: con più oggetti scelti, **G** (Montaggio > Crea gruppo) li lega; **U** (Sciogli il gruppo) li scioglie, e su un video con il suo audio in gruppo scollega l'audio. Un video e il suo audio nascono già in gruppo. Scegliendo un oggetto, i compagni di gruppo hanno il contorno **tratteggiato** e la barra di stato dice quanti sono. Trascinando un oggetto si muove tutto il gruppo dello stesso tratto, ognuno sulla sua corsia: senza Spinta si ferma contro il primo oggetto non suo, con la Spinta gli oggetti urtati scorrono davanti e i loro gruppi con loro.
  - **Dividi, elimina e taglia con i gruppi**: **S** su un oggetto di un gruppo divide tutti i membri che il cursore attraversa, e le parti dopo formano un gruppo nuovo. **Canc** con più oggetti scelti (Ctrl+clic) toglie solo quelli; gli altri del gruppo restano. Tagliare il bordo di un oggetto cambia solo lui. I compagni di gruppo hanno il contorno arancione.
  - **Scheda Gruppi** (in alto a sinistra, accanto a File del progetto e Transizioni): l'elenco dei gruppi del montaggio, con dentro i loro oggetti (video, audio, scritte); in corsivo i video con il loro audio. Un clic su un gruppo o su una voce la sceglie sulla linea del tempo e porta il cursore all'inizio; scegliendo sulla linea del tempo si evidenzia nell'elenco. Si cambiano i gruppi trascinando le voci: su un altro gruppo, nello spazio vuoto (una sola voce esce dal gruppo, due o più ne formano uno nuovo), oppure **Alt+trascina** un oggetto dalla linea del tempo su un gruppo. Modificare l'elenco non cambia mai il montaggio, solo come sono fatti i gruppi, e vale con Annulla/Ripeti. **F2** o doppio clic rinomina un gruppo (il nome si salva nel progetto; vuoto = «Gruppo N»); il tasto destro dà il menu: Rinomina, Sciogli il gruppo (Scollega l'audio per un video con il suo audio), Togli dal gruppo, Seleziona sulla timeline. L'ultima scheda aperta si ricorda; la barra di stato mostra il nome del gruppo dell'oggetto scelto.
  - **Spinta** (Montaggio > Spingi i pezzi successivi, con la spunta verde quando è accesa; di default accesa, si ricorda): trascinando un pezzo verso destra, se tocca i pezzi che seguono li spinge davanti a sé (prima consuma lo spazio nero, poi sposta il pezzo successivo e così via). Verso sinistra il pezzo si ferma contro quello che lo precede. Spento: il pezzo si ferma contro quello che lo segue, o lo supera se oltre c'è posto, e gli altri non si muovono.
  - **Scollega audio** (tasto **U** su un video con il suo audio in gruppo): l'audio del pezzo diventa una clip libera che resta sulla corsia A1 (sopra l'audio del pezzo, che viene silenziato), con la sua linea del livello; ha il livello, il muto e il solo di A1, e si trascina anche su A2, A3... e ritorno. Nel progetto sta in una traccia a parte (`suA1`). La clip si sposta e si taglia per conto suo (ritardi, audio più lungo del video) e l'esportazione la mixa. **Ricollega l'audio al pezzo** toglie la clip e rimette l'audio del pezzo, se la clip è ancora allineata (stesso file, stesso sincronismo).
  - **La linea del livello** sta **dentro ogni pezzo video (su A1) e ogni clip audio**, al centro della corsia, e li segue: se sposti, dividi o accorci il pezzo o la clip, la linea resta con loro (i punti sono nei tempi del file; la sfumatura attaccata a un bordo (l'ultimo punto sul bordo finale, o il primo sull'iniziale) lo segue: accorciando o riallungando la clip si spostano tutti i punti della sfumatura, quelli prima restano dove sono). Finché non ha punti è appena accennata (0 dB). **Un clic sulla linea aggiunge un punto**, che si trascina: in alto sale (fino a +12 dB), in basso scende (fino al silenzio, in fondo), vicino al centro si aggancia a 0 dB; in orizzontale si sposta nel tempo, restando dentro il pezzo e fra i due punti vicini. Tra un punto e l'altro il volume sale o scende in modo continuo: con due punti si fa una **dissolvenza**, con quattro si abbassa solo un tratto. Un **doppio clic su un punto lo toglie**. La linea si moltiplica al livello della traccia (il cursore a sinistra), vale nell'anteprima (anche mentre suona) e nell'esportazione, si salva nel progetto e Annulla e Ripeti la seguono. Per spostare una clip la si prende fuori dalla linea (sopra o sotto).
  - L'**esportazione** con clip sulle tracce aggiunte, o con un livello, un muto o un solo su A1, o con una linea del livello su un pezzo video, scrive l'audio **mixato** (lo stesso dell'anteprima: livelli, linee, muto e solo) e **ricodificato** in AAC 48 kHz stereo 256 kb/s; il video segue le solite regole (copia o ricodifica). La finestra di riepilogo dice "Audio: mixato e ricodificato (N file audio)" e così il messaggio finale. Senza queste modifiche l'audio resta copiato com'è. Chiudendo gli spazi vuoti all'esportazione (o quando si chiudono da soli) il tempo tolto sparisce anche dalle tracce audio aggiunte: la musica che parte con il video resta con il video. Il contenitore deve accettare l'AAC (MP4, MOV e MKV sì).
- Anche l'audio sostitutivo viene copiato senza ricodifica, quindi deve poter stare nel contenitore del file esportato: per un MP4 vanno bene AAC/M4A e MP3, non un WAV. Se non va bene, un messaggio lo dice subito (con un MKV vanno bene quasi tutti i formati).

### Annulla e Ripeti

- **Modifica > Annulla** (Ctrl+Z) torna indietro di una modifica; la voce del menu dice quale (es. "Annulla Dividi pezzo"). **Ripeti** (Ctrl+Y o Ctrl+Maiusc+Z) la rifà. Valgono per tutto ciò che cambia il progetto: aggiungere o togliere file, aggiungere, togliere (anche con X), spostare, dividere ed eliminare pezzi, svuotare il montaggio, togliere e rimettere l'audio, sostituirlo, spostarlo e tornare all'originale. IN e OUT della Sorgente no: si rimettono con un tasto.
- L'asterisco nel titolo segue Annulla e Ripeti: tornando allo stato salvato sparisce.

### Zoom della linea del tempo

- Le linee del tempo della Sorgente e del Montaggio si possono ingrandire fino a circa 12 pixel per fotogramma: **Ctrl+rotellina** ingrandisce o riduce tenendo fermo il punto sotto il mouse, **+** e **-** fanno lo stesso sulla linea del tempo dell'anteprima attiva (centrando sul cursore). Si può anche rimpicciolire oltre: i pezzi occupano solo una parte della barra (fino a un decimo) e a destra resta spazio vuoto. Il pulsante **Adatta**, in basso a destra di ogni linea del tempo, torna a mostrare tutto esattamente nella larghezza.
- Quando la linea del tempo è ingrandita compare una barra di scorrimento; si scorre anche con la rotellina (o Maiusc+rotellina).
- Durante la riproduzione la vista segue il cursore, una pagina alla volta. Se si scorre a mano smette di seguirlo, finché la riproduzione non riparte o si preme Adatta.

### Miniature

Nella timeline del montaggio ogni pezzo mostra un fotogramma all'inizio e, se il blocco è abbastanza largo, altri a distanza regolare (presi ai fotogrammi chiave: si decodifica un solo fotogramma per miniatura). Vengono preparate in background, con la GPU quando c'è, e tenute da parte: allargare la finestra o spostare i pezzi non le rifà.

### Progetto

- **Cartella di lavoro**: il salvataggio dei progetti e dell'esportazione si apre lì (all'inizio `Documenti\Editor video project`, creata se manca); si cambia da **File > Cartella di lavoro...**.
- **File > Salva progetto** (Ctrl+S) salva il lavoro in un file `.evproj`: i file usati, i pezzi del montaggio, i tratti senza audio, l'audio sostitutivo con il suo spostamento, IN e OUT della Sorgente e dove erano le anteprime. Dentro il progetto l'audio è già organizzato in **tracce audio**: A1 è l'audio dei pezzi video, e l'audio sostitutivo è una clip audio sulla traccia A2 con A1 silenziata (un progetto con tracce audio in più viene salvato nel formato 3; gli altri restano leggibili anche dalle versioni di prima). Il video e l'audio originali non vengono mai toccati: il progetto li indica soltanto. **Salva progetto con nome...** (Ctrl+Maiusc+S) ne crea una copia con un altro nome.
- **File > Apri progetto...** (Ctrl+P; Ctrl+O resta per aprire un file video) e **File > Progetti recenti** (gli ultimi 5) lo riaprono; anche `EditorVideo.exe montaggio.evproj`.
- Il progetto ricorda per ogni file sia il percorso completo sia quello relativo alla sua cartella: se si sposta tutta la cartella (progetto e video insieme, anche su un altro disco) i file si ritrovano. Se un file non c'è più, un messaggio elenca quelli mancanti: **Trova...** permette di indicare dove sono ora (gli altri mancanti vengono cercati nella stessa cartella), **Continua senza** toglie i loro pezzi dal montaggio. Se un file è cambiato (dura diversamente da quando è stato salvato il progetto) compare un avviso.
- Nel titolo della finestra c'è il nome del progetto, con un asterisco se ci sono modifiche non salvate; chiudendo il programma o aprendo un altro progetto viene chiesto se salvarle.
- Il file è JSON leggibile (UTF-8). Un montaggio con spazi vuoti si salva nella versione 2 del formato (che le versioni precedenti del programma rifiutano); senza spazi resta nella versione 1. I tempi sono numeri interi di "tick" da 1/705 600 000 di secondo, un'unità che divide esattamente le basi dei tempi di video e audio (1/90000, 1/1000, 1/48000, 1/44100, 1001/30000...): riaprendo un progetto ogni tempo torna identico. Un progetto salvato da una versione più recente del programma viene rifiutato con un messaggio, così come un file rovinato.

### Esportazione

- **File > Esporta...** (Ctrl+E) salva il montaggio in un nuovo file, nel contenitore del primo pezzo: propone `<nome>_montaggio.<estensione>` nella cartella di lavoro. Se il montaggio è vuoto esporta il solo IN-OUT della Sorgente (`<nome>_taglio.<estensione>`).
- **Senza ricodifica**: video e audio vengono copiati così come sono (stessi codec, stessa qualità, nessuna perdita) nello stesso contenitore dell'originale (MP4 resta MP4, MKV resta MKV...). Per questo è anche molto veloce.
- **Taglio preciso**: il video parte esattamente dal fotogramma di IN e finisce a OUT. Senza ricodifica un video può iniziare solo da un fotogramma chiave, quindi la parte fra IN e il primo fotogramma chiave dopo si ricodifica (nel formato del progetto, con la scheda video) e il resto si copia; se il file ha B-frame, anche la parte fra l'ultimo fotogramma chiave e OUT. Se fra IN e OUT non c'è un fotogramma chiave si ricodifica tutto il pezzo. La finestra prima di esportare dice quanti tagli precisi ci sono e quanto durano le parti ricodificate (di solito meno di un secondo l'una). Se il formato del progetto non si sa ricodificare (codec diverso da H.264 e HEVC, o nessun encoder sul PC) IN va al fotogramma chiave prima, come senza ricodifica, e la finestra lo dice.
- L'audio parte insieme al video; la durata finale è quella mostrata sotto la linea del tempo (con al massimo qualche centesimo di differenza). Si copiano video e audio; sottotitoli e altri flussi per ora no.
- **Spazi vuoti**: nel file esportato diventano **nero e silenzio**, lunghi come nel montaggio (arrotondati ai fotogrammi), anche prima del primo pezzo: il file dura quanto il montaggio nell'anteprima. Il nero si crea nel formato del progetto con la scheda video, come la ricodifica, e l'audio è silenzio nel formato dell'audio del progetto (AAC). Nella finestra prima di esportare c'è **Chiudi gli spazi vuoti** per unire invece i pezzi uno dopo l'altro. Se il formato del progetto non si sa ricodificare (codec diverso da H.264 e HEVC, o nessun encoder) gli spazi si chiudono e la finestra lo dice.
- **Ricodifica**: i pezzi segnati con R vengono decodificati e ricodificati nel formato del progetto con la scheda video (NVIDIA NVENC, oppure Media Foundation di Windows): stessa risoluzione (con bande nere se le proporzioni sono diverse), stessi fotogrammi al secondo (i fotogrammi si ripetono o si saltano: 30 → 60, 59,71 → 60, frequenza variabile, senza che il video si sposti rispetto all'audio), stesso formato dei pixel. L'audio di un pezzo ricodificato si copia se è uguale a quello del formato, altrimenti si ricodifica in AAC (silenzio se il file non ha audio). Gli altri pezzi restano copiati così come sono: si ricodifica solo quello che serve. Per ora la ricodifica si fa in H.264 e HEVC; se il formato del progetto è un altro codec, o sul PC non c'è un encoder, un messaggio lo dice.
- Prima di esportare il montaggio una finestra riassume **quanti pezzi si copiano e quanti si ricodificano** (con la durata), e permette di scegliere la **qualità della ricodifica** (Alta o Media) e **Ricodifica tutto** (anche i pezzi che si potrebbero copiare: più lento, per la massima compatibilità con i lettori). Durante l'esportazione la finestra di avanzamento dice quanto tempo manca; **Annulla** la ferma e cancella il file a metà.
- **Parametri nel flusso**: se i pezzi H.264 o HEVC hanno parametri di codifica diversi (segno P), il file esportato ripete i parametri di ogni pezzo (SPS e PPS, per HEVC anche VPS) davanti a ogni suo fotogramma chiave; nei file MP4 il video è dichiarato "avc3"/"hev1" invece di "avc1"/"hvc1". I pacchetti restano copiati così come sono. Se tutti i pezzi hanno gli stessi parametri il file è come prima.
- Con più tratti, nel file creato si susseguono senza buchi: a ogni giunzione video e audio ripartono insieme. L'audio PCM (es. WAV) viene tagliato al campione esatto; l'audio compresso (AAC, MP3...) a pacchetti, quindi a ogni giunzione può mancare al massimo un pacchetto (circa 20 ms).
- L'originale non viene mai modificato. Se si annulla o c'è un errore, non resta un file a metà.

- Il video viene decodificato dalla **scheda video (D3D11VA)** quando il codec lo permette (H.264, HEVC, VP9, AV1, MPEG-2... secondo la GPU), altrimenti dal **processore**: il passaggio è automatico, anche se la GPU dà errore a metà. In basso a destra c'è scritto quale si sta usando (`Decodifica: GPU (D3D11VA)` / `Decodifica: software`).
- L'audio è il riferimento per la sincronizzazione: ogni fotogramma viene mostrato quando l'audio arriva al suo tempo. Se il file non ha audio (o non c'è una scheda audio) si usa l'orologio di sistema.
- **Ricodifica** (in preparazione, per ora non cambia l'esportazione): il programma sa codificare video H.264, HEVC e AV1 con la scheda video (**NVIDIA NVENC**, oppure **Media Foundation** di Windows, che usa la scheda video o il codificatore software di Windows) a qualità costante, e audio AAC. All'avvio prova, in background, quali encoder funzionano davvero su questo PC codificando un fotogramma con ognuno: l'elenco è in **Aiuto > Informazioni**. La build di FFmpeg resta LGPL (niente x264/x265).
- L'audio esce dalla scheda audio predefinita di Windows (waveOut), così non serve il modulo Qt Multimedia.

## Test

- Esegui `EditorTests` (progetto di avvio o da `bin\x64\Debug\EditorTests.exe`), oppure usa Esplora test di Visual Studio.
- I test generano al volo piccoli video di prova (MKV, MP4, WAV) con FFmpeg nella cartella temporanea: nel repository non ci sono file binari. Con la variabile d'ambiente `EDITORVIDEO_KEEP_SAMPLES=1` i file dei test di analisi restano in `%TEMP%\EditorVideoTests`.
- Per provare l'anteprima senza file propri: `EditorTests.exe --gtest_also_run_disabled_tests --gtest_filter=*GeneraVideoDimostrativi*` crea in `%TEMP%\EditorVideoDemo` due video da 10 secondi (uno MPEG-2, che di solito la GPU decodifica, e uno MPEG-4 Part 2, sempre in software) e un file solo audio. L'audio è muto.
- I test `Encoders.*` codificano 5 secondi di video (alta e media qualità, con fotogrammi chiave chiesti) e audio AAC con ogni encoder funzionante, poi rileggono il file e controllano durata, numero di fotogrammi, fotogrammi chiave e luminosità; scrivono quali encoder hanno trovato. Senza nessun encoder video (es. un server senza scheda video né Media Foundation) le prove video vengono saltate con un messaggio.
- `EditorVideo.exe --smoke-test` verifica che l'applicazione parta con tutte le DLL; `EditorVideo.exe --smoke-test file.mkv` riproduce anche il file per 2 secondi, controlla che la posizione avanzi col tempo giusto, segna due tratti e li esporta uniti dalla finestra (controllando anche che la finestra di avanzamento si chiuda) (con `EDITORVIDEO_SMOKE_SCREENSHOT=immagine.png` salva anche un'immagine della finestra). Con `EDITORVIDEO_NO_AUDIO=1` si simula un PC senza scheda audio.
- `EditorVideo.exe --export-test origine destinazione IN OUT [IN OUT ...]` (tempi in secondi) esporta uno o più tratti uniti come l'interfaccia (taglio preciso se si può, altrimenti IN al fotogramma chiave prima) e controlla il file creato (codici di uscita: 0 tutto giusto, 2 errore, 3 controllo non riuscito).
- `EditorVideo.exe --join-test destinazione file1 file2 [...]` unisce i file interi come l'interfaccia, dopo il controllo di compatibilità, e controlla il file creato, decodificando tutti i fotogrammi (nessun errore del decoder, anche alle giunzioni) (codici di uscita: 0 tutto giusto, 4 file non compatibili, 2 errore, 3 controllo non riuscito). Anche `--sequence-test` decodifica tutto il file creato.
- `EditorVideo.exe --codec-info file1 file2 [...]` scrive codec e parametri di codifica di ogni file e, per ogni file dopo il primo, se rispetto al primo si copia, si copia con i suoi parametri nel flusso o va ricodificato (con i motivi).
- I test `BlackGaps.*` (con la scheda video) esportano un montaggio con spazi vuoti all'inizio, in mezzo e brevissimi: gli spazi devono essere neri (luminanza fotogramma per fotogramma), il resto i fotogrammi giusti, il file lungo quanto il montaggio con l'audio continuo, senza errori di decodifica, in MP4 e MKV; con "Chiudi gli spazi vuoti" i pezzi restano attaccati. `ClosedGapsTest` (anche nella build automatica): con un formato che non si sa ricodificare gli spazi si chiudono e il riepilogo lo dice.
- I test `PreciseCut.*` (con la scheda video) tagliano file H.264 fra due fotogrammi chiave: il file esportato deve partire esattamente dal fotogramma di IN (controllato sulla luminosità di ogni fotogramma), finire a OUT anche con i B-frame, avere l'audio lungo quanto il video e senza sovrapposizioni, e decodificarsi senza errori, in MP4 e MKV.
- I test `Reencode.*` (con la scheda video: senza, vengono saltati) montano in un progetto H.264 1080p60 un pezzo 720p30, uno 640x480 (4:3) con audio a 44,1 kHz, uno HEVC senza audio e due pezzi copiati, in MP4 e MKV: controllano durata, audio continuo e lungo quanto il video, tutti i fotogrammi decodificati senza errori e nell'ordine giusto (quelli a 30 al secondo ripetuti), le bande nere, i pacchetti copiati identici; poi "Ricodifica tutto", Annulla a metà (niente file) e un formato preso da un altro file. `--join-test` e `--sequence-test` ricodificano anche loro i pezzi diversi e scrivono quanti tratti sono copiati e quanti ricodificati.
- I test `ParameterJoin.*` codificano due file H.264 con parametri diversi (con la scheda video: senza, vengono saltati), li uniscono in MP4 e MKV e controllano che tutti i fotogrammi si decodifichino senza errori e che i pacchetti copiati siano identici agli originali.
- `EditorVideo.exe --sequence-test destinazione [spazio:S] file IN OUT [[spazio:S] file IN OUT ...]` costruisce un montaggio con i pezzi in quell'ordine (con `spazio:S` uno spazio vuoto di S secondi prima del pezzo), lo esporta e controlla durata e ordine (all'inizio di ogni pezzo lo stesso fotogramma dell'originale, a metà di ogni spazio un fotogramma nero).
- `EditorVideo.exe --audio-test destinazione file [mute DA A]... [replace audio OFFSET] [omit]` mette il file intero nel montaggio, toglie l'audio dai tratti indicati e/o lo sostituisce, esporta e controlla durata, codec, che l'audio sia continuo (nessun buco nuovo fra un blocco e l'altro) e che nei tratti ci sia silenzio. Codice 4 se l'audio sostitutivo non può stare nel contenitore.
- `EditorVideo.exe --thumbnail-test file [file ...]` estrae una miniatura per ogni fotogramma chiave (al massimo 20 per file), come la timeline, e scrive quanto ci vuole e con quale decodifica.
- `EditorVideo.exe --project-test file [file ...]` prova i progetti con file veri, in una cartella temporanea: salva un montaggio (con audio tolto e sostituito), lo riapre e lo risalva (deve essere identico), sposta la cartella (i file devono ritrovarsi), esporta il progetto riaperto, poi toglie un file (deve risultare mancante) e prova un progetto di versione sconosciuta e uno rovinato (devono essere rifiutati).
- `EditorVideo.exe --export-dialog-test progetto.evproj destinazione` prova l'esportazione come l'utente: apre il progetto, preme Esporta, conferma il riepilogo, sceglie il file nella finestra di Windows e chiude il messaggio finale (un altro thread guida le finestre, senza mouse) e scrive dove si ferma. `--project-export-test progetto.evproj destinazione` esporta il montaggio del progetto dalla finestra, senza le finestre di dialogo. Codice 0 se riesce, 3 se si blocca.
- `--smoke-test` con uno o più file costruisce un montaggio dall'interfaccia, prova l'anteprima Montaggio a cavallo di una giunzione e lo esporta. Con la variabile d'ambiente `QT_QPA_PLATFORM=minimal` le prove girano senza aprire finestre.
- A ogni push GitHub Actions compila la soluzione in Release, lancia i test, avvia l'applicazione con `--smoke-test`, prova la riproduzione dei file dimostrativi, l'esportazione di uno e più tratti e l'unione di più file (anche il rifiuto di file non compatibili).
