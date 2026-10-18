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

Gli eseguibili finiscono in `bin\x64\<Configurazione>\`.

## Test

- Esegui `EditorTests` (progetto di avvio o da `bin\x64\Debug\EditorTests.exe`), oppure usa Esplora test di Visual Studio.
- A ogni push GitHub Actions compila la soluzione in Release, lancia i test e avvia l'applicazione con `--smoke-test` per verificare che parta con tutte le DLL.
