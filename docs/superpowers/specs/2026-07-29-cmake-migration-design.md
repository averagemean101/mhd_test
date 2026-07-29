# Migrazione del build system da MSBuild a CMake

Data: 2026-07-29

## Obiettivo

Sostituire `mhd_test.sln` + `mhd_test.vcxproj` con una build CMake, verificarla, e poi
eliminare i file MSBuild dal repository.

## Baseline misurato prima di toccare nulla

Entrambe le configurazioni MSBuild sono state buildate e smoke-testate il 2026-07-29,
con questo esito:

| config | link libmicrohttpd | exe | runtime accanto all'exe | smoke test |
|---|---|---|---|---|
| Debug x64 | statico (`Debug-static/libmicrohttpd_d.lib`) | 1.393.152 B | nessuno | `exit=0` |
| Release x64 | DLL (`Release-dll/libmicrohttpd-dll.lib`) | 156.160 B | `libmicrohttpd-dll.dll` | `exit=0` |

Warning di compilazione nel baseline: **zero**, in entrambe le configurazioni, su compilazione
completa (non incrementale). Questo è il metro di confronto per la build CMake.

Lo smoke test è: `"\n" | mhd_test.exe` con `ffmpeg\bin` nel `PATH`, che deve stampare
`Server started on port 8080.` / `Server stopped.` e uscire con codice 0. Serve lo stdin aperto
perché `WebServer::start()` blocca su `getc(stdin)`.

## Vincoli scoperti nel progetto esistente

1. **`common/avpp.cpp` fa `#include "stdafx.h"`, e quell'header sta in `mhd_test/`.** Si risolve
   solo perché l'`IncludePath` del `.vcxproj` contiene `.\`, cioè la directory del progetto.
   La directory `mhd_test/` deve restare sull'include path del target, altrimenti non compila
   un file che vive in un altro repository.
2. **Il PCH configurato è un errore.** `PrecompiledHeader=Create` è impostato su entrambe le TU
   senza `PrecompiledHeaderFile`: produce due `/Yc` senza through-header che scrivono lo stesso
   `$(IntDir)$(TargetName).pch`. Funziona solo perché la build è seriale.
3. **La variante statica di libmicrohttpd è compilata contro il CRT statico.** Da qui
   `IgnoreSpecificDefaultLibraries` con `libcmtd`/`libcmt`: il progetto usa il CRT DLL (`/MD`).
4. **Il sorgente è Windows-only**: `stdafx.h` include `<conio.h>`. CMake non rende la build
   portabile su Linux; servirebbe lavoro sul codice. Fuori dallo scope di questa migrazione.
5. `MHD_W32LIB` / `MHD_W32DLL` non sono definite da nessuna parte. Per l'header è solo un
   acceleratore di link, quindi oggi funziona comunque.

## Decisioni

| tema | scelta | motivo |
|---|---|---|
| link libmicrohttpd | ~~statico in entrambe~~ → **DLL in entrambe**, con copia post-build | il link statico è impraticabile, vedi "Ipotesi falsificate". La simmetria Debug/Release, che era l'obiettivo, si ottiene comunque |
| standard C++ | **C++17** (era `stdcpplatest`) | richiesta esplicita in corso d'opera |
| generatore | **Visual Studio 17 2022**, toolset v143, x64 | conserva il debug F5 in Visual Studio |
| configurazioni | solo `Debug` e `Release` | le uniche mai esistite; `Win32`/`x86` spariscono per costruzione |
| percorsi dipendenze | cache variables con default relativi | zero configurazione nel layout atteso, ma sovrascrivibili |
| PCH | **eliminato**, non sostituito | su 2 TU che già includono `stdafx.h` in prima riga non paga |
| commit | **da fare a mano dopo revisione** | richiesta esplicita dell'utente |

Conseguenza accettata del generatore VS: CMake **rigenera** un `.sln` e un `.vcxproj` dentro
`build/`. Escono dal controllo versione e nessuno li edita più a mano, ma restano sul disco.

## Struttura

```
mhd_test/
├── CMakeLists.txt        <- nuovo, unico
├── CMakePresets.json     <- nuovo, entry point al posto della solution
├── mhd_test/             <- sorgenti invariati
└── build/                <- gitignorato: .sln generato, oggetti, exe
```

Le tre dipendenze diventano cache variables, ognuna validata in configure con un errore che dice
cosa manca e dove:

- `FFMPEG_ROOT` → `${CMAKE_SOURCE_DIR}/../ffmpeg`
- `COMMON_ROOT` → `${CMAKE_SOURCE_DIR}/../common`
- `MHD_ROOT` → `${CMAKE_SOURCE_DIR}/third_party/libmicrohttpd-0.9.73-w32-bin/x86_64/VS2019`

libmicrohttpd è un `STATIC IMPORTED` target con `IMPORTED_LOCATION_DEBUG`/`_RELEASE` sulle due
varianti statiche e `MHD_W32LIB` come compile definition. FFmpeg è una `INTERFACE` library con le
stesse 6 import lib di oggi (`swresample` non era linkata e non viene aggiunta); le sue DLL
continuano a risolversi dal `PATH`.

## Mappatura dei flag

Dove CMake ha già il default corretto non si aggiunge nulla.

| MSBuild | CMake |
|---|---|
| `/MD`, `/MDd` | default di `MSVC_RUNTIME_LIBRARY` |
| `/W3`, subsystem Console | default CMake |
| `/sdl`, `/permissive-`, `UNICODE;_UNICODE`, `_CONSOLE` | espliciti |
| `stdcpplatest` | `CXX_STANDARD 17` + `CXX_STANDARD_REQUIRED` (scelta deliberata, non fedeltà) |
| `WholeProgramOptimization` | `INTERPROCEDURAL_OPTIMIZATION_RELEASE` |
| `/Gy`, `/Oi`, `/OPT:REF`, `/OPT:ICF` | espliciti su Release |
| pdb anche in Release | `/Zi` + `/DEBUG` su Release |
| `IgnoreSpecificDefaultLibraries` | eliminato: serviva al CRT statico della lib statica |
| `PrecompiledHeader=Create` | eliminato |

## Criterio di accettazione, ed esito

Prima di cancellare i file MSBuild, tutti e quattro i punti. Verificati il 2026-07-29:

| # | criterio | esito |
|---|---|---|
| 1 | configure pulito | ok |
| 2 | build `Debug` e `Release`: 0 errori, nessun warning nuovo sul baseline di 0 | ok, 0 warning |
| 3 | smoke test su entrambi gli exe: output atteso e `exit=0` | ok su entrambi |
| 4 | `dumpbin /dependents` coerente con il link scelto | ok, vedi sotto |

Sul punto 4 il confronto è stato fatto contro i binari MSBuild **prima** di cancellarli:

- **Release**: insieme di dipendenze **identico** a quello della build MSBuild.
- **Debug**: differisce esattamente come previsto dal cambio di link. La MSBuild importava
  `WS2_32.dll` direttamente, perché la libmicrohttpd statica se la portava dentro; la CMake importa
  `libmicrohttpd-dll_d.dll` e ws2_32 resta dentro la DLL.

Nota: nelle dipendenze compaiono 5 DLL FFmpeg, non 6. `swscale.lib` è linkata — com'era anche nel
`.vcxproj` — ma nessun simbolo viene usato, quindi il linker scarta l'import. Vale per entrambe le
build, quindi non è una differenza introdotta dalla migrazione.

Artefatti prodotti:

| config | exe | DLL affiancata |
|---|---|---|
| Debug | 1.029.120 B | `libmicrohttpd-dll_d.dll` (378.880 B) |
| Release | 160.768 B | `libmicrohttpd-dll.dll` (138.240 B) |

## Ipotesi falsificate durante l'implementazione

**1. "libmicrohttpd statica in entrambe le configurazioni."** Falsificata da un vincolo del
toolchain. `Release-static/libmicrohttpd.lib` contiene oggetti LTCG prodotti dal compilatore
VS2019, e linkarli assieme ai nostri oggetti VS2022 in `/GL` fallisce:

```
LINK : fatal error C1047: l'oggetto o il file di libreria '...libmicrohttpd.lib' è stato creato
con una versione del compilatore diversa rispetto ad altri oggetti
```

Disattivando `/GL` da parte nostra il link passa, ma al prezzo di perdere la whole program
optimization **e** di 13 `LNK4099`, perché la distribuzione `Release-static` non contiene il
`.pdb` (mentre `Debug-static` sì). Contro un baseline di 0 warning è una bocciatura.

Questo spiega retroattivamente l'asimmetria del `.vcxproj`: Release usava la DLL perché la variante
statica **non è linkabile con WPO attiva**, non per una scelta stilistica.

Esito: **DLL in entrambe le configurazioni**, con `add_custom_command(POST_BUILD)` che copia la
variante per-configurazione via `$<TARGET_FILE:microhttpd>`. Si ottiene la simmetria che era
l'obiettivo, si conserva la WPO, e la copia non si fa mai più a mano. Cadono anche i
`/NODEFAULTLIB:libcmt[d]`, che servivano solo al CRT statico dentro la lib statica.

**2. "Il passaggio a C++17 è una semplice sostituzione di flag."** Falsificata: `common/utils.h`
usa `std::stringstream` e `common/ThreadSafeQueue.h` usa `std::optional` senza includere
`<sstream>` e `<optional>`. Sotto `/std:c++latest` arrivavano per inclusione transitiva; a
`/std:c++17` no, e la build produce ~30 errori. È un difetto latente di `common`, mascherato dal
vecchio flag.

Esito: i due include vengono aggiunti a `mhd_test/stdafx.h`, con il motivo scritto accanto.
Non si tocca `common/`, che è un repository separato condiviso con il progetto `v`.

## Fuori scope

- Portabilità Linux (richiede rimuovere `conio.h` e il path hardcoded `d:\downloads\www\`).
- I difetti noti elencati nel README (log di avvio prima di `start()`, blocco su `getc(stdin)`,
  fallback di `streaming_core` su una sorgente morta).
- Aggiungere test automatici: non ne esistono, e introdurli è un progetto separato.

## Modifiche collaterali

- **README**: riscrittura della sezione Build; la riga su `libmicrohttpd-dll.dll` accanto all'exe
  diventa "la copia la fa CMake", e cade il difetto noto sulle configurazioni `Win32`/`x86`.
  Documentati anche i due vincoli scoperti: perché non si alza lo standard e perché non si linka
  la variante statica.
- **`mhd_test/stdafx.h`**: aggiunti `<sstream>` e `<optional>`. Unica modifica a un sorgente,
  richiesta dal passaggio a C++17.
- **.gitignore**: aggiunta di `/build/`, rimozione di `x64/` e `Win32/`.
- **Eliminati**: `mhd_test.sln`, `mhd_test/mhd_test.vcxproj`, `mhd_test/mhd_test.vcxproj.filters`,
  e le directory di output `x64/` ormai orfane.
- **`.vscode/settings.json`**: non viene creato. Con un generatore multi-config
  `-DCMAKE_BUILD_TYPE=${buildType}` è ignorato da CMake, quindi sarebbe una riga inerte.
