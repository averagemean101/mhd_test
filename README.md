# mhd_test — web TV in C++ su FFmpeg + libmicrohttpd

Server HTTP che apre uno stream live, lo rimuxa in **MP4 frammentato in memoria** e lo serve a un
browser via `<video>`, senza mai passare da file.

I tre mattoncini vengono da [`common`](https://github.com/averagemean101/common): `avpp` (wrapper
RAII su FFmpeg), `ThreadsafeQueue` e `HttpServer` (wrapper su libmicrohttpd).
`format_open_output_to_buffer` spinge i byte muxati nella coda e lo `stream_generator` di MHD li
estrae nella risposta HTTP.

## Dipendenze

Una è versionata qui dentro, due no:

| dipendenza | dove | perché |
|---|---|---|
| **libmicrohttpd 0.9.73** | `third_party/`, nel repo | build prebuilt ufficiale per Windows, non c'è un upstream da tracciare: pinnare i binari esatti è il punto |
| **`common`** | repo fratello, `../common` | è codice condiviso con il progetto `v` e sviluppato in parallelo. Tenerne **una sola copia** sul disco evita di modificare quella sbagliata |
| **FFmpeg** | `../ffmpeg` | ~1,5 GB di binari di terze parti, aggiornati per conto loro |

Il `.vcxproj` le risolve per percorso relativo, quindi la struttura attesa è:

```
<workspace>/
├── mhd_test/     <- questo repo
├── common/       git clone https://github.com/averagemean101/common.git
└── ffmpeg/       build win64-gpl-shared da https://github.com/BtbN/FFmpeg-Builds
                  (cartella o symlink, con dentro include/ lib/ bin/)
```

Di libmicrohttpd è versionata **solo** la variante `x86_64/VS2019`: le altre (x86, MinGW, VS2017)
restano sul disco ma fuori dal repo, perché il perimetro supportato è solo win64. L'archivio
completo è `libmicrohttpd-0.9.73-w32-bin.zip` da <https://ftp.gnu.org/gnu/libmicrohttpd/>.

## Build

Toolchain: **Visual Studio 2022 Build Tools, toolset v143, solo x64**. Niente CMake, niente Ninja.

```powershell
msbuild mhd_test.sln /p:Configuration=Release /p:Platform=x64
```

L'eseguibile esce in `x64\Release\`. `libmicrohttpd-dll.dll` deve stare accanto all'exe; le DLL di
FFmpeg si risolvono dal PATH (`ffmpeg\bin`).

**Attenzione ai soname di FFmpeg**: cambiano a ogni aggiornamento dello snapshot master (al
28/07/2026: `avcodec-63 / avformat-63 / avutil-61 / avfilter-12`). Dopo un aggiornamento va
ricompilato, altrimenti l'eseguibile non parte affatto.

## Uso

```
mhd_test.exe        # server sulla porta 8080, ENTER per chiudere
```

- `GET /live/<canale>` → MP4 frammentato in streaming, `Content-Type: video/mp4`
- `GET /<path>` → file server su `d:\downloads\www\`

## Stato delle sorgenti (28/07/2026)

| canale | esito |
|---|---|
| **`tv8`** | **funziona**, verificato in Firefox. La `.php` conia un token nuovo a ogni chiamata, quindi va invocata quella e non la URL che restituisce |
| `rai1` | **403** dall'edge Akamai. Recuperabile ma serve codice: User-Agent da browser, `&output=64` (senza il quale risponde 200 con un `<Mediapolis>` vuoto) e il parsing dell'XML, perché la URL arriva in CDATA |
| `italia1`, `focus` | **morti**: playlist Mediaset salvate nel 2021, il cui CDN non risolve più in DNS |

Il fallback di `streaming_core` punta a `focus`, cioè a una sorgente morta: chi chiede
`/live/<qualcosa>` senza scrivere `tv8` non ottiene nulla. E `main()` avvia comunque un
`HLSGenerator` cablato su Rai, che fallisce tre volte e **ritarda di ~10 s** l'avvio del web server.

## Difetti noti, non corretti

- `main()` stampa `Server started on port 8080.` **prima** di chiamare `start()`: se la porta è
  occupata il log mostra comunque un avvio riuscito.
- `WebServer::start()` blocca su `getc(stdin)`: con stdin a EOF il server si spegne subito. Per
  pilotarlo da script serve tenere stdin aperta e mandare `\n` per la chiusura pulita — che è anche
  l'unico modo per far flushare `std::cout` quando è rediretto su file.
- La solution porta ancora le configurazioni `Win32`/`x86`, fuori dal perimetro supportato.
