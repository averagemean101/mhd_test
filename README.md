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

Il `CMakeLists.txt` le risolve per percorso relativo, quindi la struttura attesa è:

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

Toolchain: **Visual Studio 2022, toolset v143, solo x64**, guidata da CMake ≥ 3.21. Il preset
`win64` è l'unica configurazione supportata e fissa generatore, toolset e architettura.

```powershell
cmake --preset win64                  # una volta: genera build\
cmake --build --preset win64-release   # oppure win64-debug
```

Gli eseguibili escono in `build\Release\` e `build\Debug\`. La `libmicrohttpd-dll.dll` della
variante giusta viene copiata accanto all'exe da un post-build di CMake, quindi non c'è nulla da
sistemare a mano; le DLL di FFmpeg si risolvono dal PATH (`ffmpeg\bin`), e per il debug in Visual
Studio ci pensa `VS_DEBUGGER_ENVIRONMENT`.

Se una dipendenza non è dove il layout la aspetta, il configure si ferma dicendo quale manca. I tre
percorsi sono sovrascrivibili: `-DFFMPEG_ROOT=<path>`, `-DCOMMON_ROOT=<path>`, `-DMHD_ROOT=<path>`.

CMake genera dentro `build\` una solution e un `.vcxproj`, utili per il debug con F5 ma
**artefatti**: non sono versionati e non vanno editati a mano.

Lo standard è **C++17**. Non alzarlo per far compilare qualcosa: `common/utils.h` e
`common/ThreadSafeQueue.h` usano `std::stringstream` e `std::optional` senza includerli, e
`stdafx.h` sopperisce esplicitamente.

Non si linka la variante statica di libmicrohttpd: `Release-static\libmicrohttpd.lib` contiene
oggetti LTCG di VS2019, che con i nostri oggetti VS2022 danno `fatal error C1047`. Rinunciando a
`/GL` il link passa, ma si perde la whole program optimization e ogni oggetto della lib emette
`LNK4099`, perché quella distribuzione non contiene il `.pdb`.

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

### HTTPS dietro un proxy con TLS inspection

Con la VPN aziendale attiva (Cato Networks SASE) **nessuna sorgente HTTPS funziona**, e il sintomo
non dice cosa sta succedendo:

```
[tls @ ...] Creating security context failed (0x80092012)
```

`0x80092012` è `CRYPT_E_NO_REVOCATION_CHECK`: il proxy re-firma ogni connessione con un
certificato effimero, che per costruzione non ha una CRL/OCSP raggiungibile. FFmpeg è compilato
solo con `--enable-schannel` e sul fallimento della verifica di revoca fa **hard-fail**, mentre
Firefox (NSS) e .NET fanno soft-fail: è per questo che la stessa URL si apre in Firefox e in
PowerShell ma non qui, e la differenza non è la rete né il momento.

Cose verificate che **non** risolvono:

- installare la CA Cato — è già in `LocalMachine\Root` e `CurrentUser\Root`: la fiducia non è il
  problema, la catena si costruisce e fallisce *solo* sulla revoca;
- passare `-ca_file` con quella CA — `tls_schannel.c` non la legge nemmeno, stesso `0x80092012`;
- un setting di sistema o una variabile d'ambiente — non esistono. FFmpeg passa
  `SCH_CRED_REVOCATION_CHECK_CHAIN` in `AcquireCredentialsHandle` e omette
  `SCH_CRED_IGNORE_NO_REVOCATION_CHECK`, che è l'unico flag che renderebbe l'errore non fatale, e
  sono flag di credenziale applicativi, non knob di registry. Confermato dalla macchina: nessuna
  policy di chain-engine configurata, la severità la chiede FFmpeg;
- `-tls_verify 0` da riga di comando o nel dizionario di `avformat_open_input` — sblocca solo
  l'open di primo livello. `ffio_copy_url_options()` propaga ai figli una whitelist fissa di nomi
  (`headers`, `user_agent`, `cookies`, `http_proxy`, `referer`, `rw_timeout`, `icy`,
  `prefer_libcurl`) in cui nessuna opzione TLS compare.

Il rimedio è l'opzione **`insecure_tls`** di `avpp::format_open_input`, che installa un
`AVFormatContext::io_open` custom: `open_url()` di hls.c instrada **ogni** playlist e segmento
attraverso quel callback, che è quindi il solo punto da cui l'opzione può raggiungere le
connessioni annidate. Verificato con la VPN attiva: `GET /live/tv8` risponde `200 video/mp4` e
consegna un init segment fMP4 valido, senza nessun `0x80092012`.

È **opt-in** e va lasciata tale: disattiva la verifica del peer su tutte le connessioni di quello
stream. Dietro un proxy che ispeziona già il traffico il downgrade è più nominale che sostanziale,
ma resta un downgrade. Fuori dalla VPN aziendale l'opzione va tolta.

## Difetti noti, non corretti

- `main()` stampa `Server started on port 8080.` **prima** di chiamare `start()`: se la porta è
  occupata il log mostra comunque un avvio riuscito.
- `WebServer::start()` blocca su `getc(stdin)`: con stdin a EOF il server si spegne subito. Per
  pilotarlo da script serve tenere stdin aperta e mandare `\n` per la chiusura pulita — che è anche
  l'unico modo per far flushare `std::cout` quando è rediretto su file.
