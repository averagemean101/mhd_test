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

Poi <http://127.0.0.1:8080/>, che serve la pagina con il link per avviare lo stream.

| rotta | risposta |
|---|---|
| `GET /` | `www/index.html`: lista canali a sinistra, televisore a destra |
| `GET /live/{rai1,rai2,tv8}` | MP4 frammentato in streaming, `Content-Type: video/mp4` |
| `GET /live/<altro>` | `404` |
| `GET /<path>` | file server, oppure l'elenco della cartella se `<path>` è una directory senza `index.html` |
| fuori dal root | `403` |

I canali stanno nella tabella `CHANNELS` di [mhd_test.cpp](mhd_test/mhd_test.cpp): aggiungerne uno è una riga lì più un `<button>` in `www/index.html`. `LivePage` risolve lo slug e passa la URL già risolta al generator, che quindi non ripete la ricerca.

Il root del file server è la cartella **`www/` accanto all'eseguibile**, non la working directory:
il programma si lancia tipicamente come `.\build\Release\mhd_test.exe` dalla radice del repo, e un
path relativo alla CWD cercherebbe nel posto sbagliato. `www/` è versionata qui e CMake la copia
accanto all'exe a ogni build, quindi **dopo aver editato la pagina serve un rebuild**.

## Stato delle sorgenti (29/07/2026)

Tre canali, tutti verificati end-to-end attraverso l'applicazione (25 MB, 24 MB e 8 MB di stream
consegnati in ~20 s ciascuno):

| canale | sorgente | note |
|---|---|---|
| `tv8` | `mytivu.it/Application/Channels/TV8.php` | la `.php` conia un token Akamai nuovo a ogni chiamata: va invocata quella, **non** la URL che restituisce |
| `rai1` | relinker Rai, `cont=2606803` | vedi sotto |
| `rai2` | relinker Rai, `cont=308718` | vedi sotto |

### Il relinker Rai non richiede parsing

Una versione precedente di questo README dava Rai per irrecuperabile senza «User-Agent da browser,
`&output=64` e il parsing dell'XML, perché la URL arriva in CDATA». **Il parsing non serve**: con

```
?cont=<id>&output=7&forceUserAgent=raiplayappletv
```

il relinker risponde con un **302 diretto** al playlist HLS, e FFmpeg segue i redirect. Con
`output=64` invece restituisce un documento `<Mediapolis>` con la URL sepolta in CDATA — quella è
la forma che richiederebbe di scriverne il parser. Senza nessun `output` risponde 200 con un
`<Mediapolis>` vuoto, che è l'origine dell'equivoco.

Resta necessario uno **User-Agent da browser**: senza, il relinker risponde `403 Access Denied`.
Ed è l'unica opzione HTTP che `ffio_copy_url_options()` propaga ai contesti annidati, quindi
impostarla una volta copre playlist e segmenti — a differenza di `tls_verify`, che ha bisogno
dell'hook `io_open`.

Verificato che gli stessi ID valgono anche per `rai3` (`cont=308709`): aggiungerlo è una riga.
Attenzione invece agli ID indovinati: `cont=2606805` risponde con un `podcastcdn/.../2606805.mp4`,
cioè **VOD**, non il canale live.

### Sorgenti rimosse

| ex canale | perché |
|---|---|
| `italia1`, `focus` | **morti**: playlist Mediaset salvate nel 2021, il cui CDN non risolve più in DNS |
| `Cielo` (mytivu) | l'endpoint esiste ma risponde `302` senza `Location` utile |

Con le prime due sono spariti `DASHGenerator` e `HLSGenerator`, che erano già disattivati,
puntavano a quelle sorgenti e cablavano dei `filesystem::current_path("d:\...")` — cambiando la
working directory **del processo**, quindi anche la risoluzione del file server.

### DNS ballerino su macchina multi-homed

Con VPN aziendale, Ethernet e Wi-Fi attive contemporaneamente ci sono **due default route e due set
di resolver**, e l'host CDN `hlslive-web-gcdn-skycdn-it.akamaized.net` a volte non risolve:

```
[tcp @ ...] Failed to resolve hostname hlslive-web-gcdn-skycdn-it.akamaized.net: The name does not resolve
av_error: code=-5, text="I/O error"
```

È intermittente — verificato fallire 3 volte di fila e poi risolvere 5 su 5 pochi minuti dopo — e
non è un problema dell'applicazione: lo stream muore perché la sorgente diventa irraggiungibile a
metà. Per questo `max_retries` è **6** con backoff crescente (~8 s di finestra): con i 3 tentativi a
500 ms fissi di prima lo stream si arrendeva dopo ~1,5 s, troppo poco per scavalcare un buco del
resolver.

Il rovescio della medaglia è dichiarato: su una sorgente **davvero** morta il client aspetta ~19 s
prima di ricevere l'errore, contro ~2 s di prima.

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
- **`LivePage::createResponse` blocca fino a 10 s prima di rispondere.** `Run()` attende che il
  thread pubblichi `media_in` per 100 × 100 ms, e solo dopo MHD manda gli header: su una sorgente
  lenta il browser resta sullo spinner senza sapere nulla. Da quando `Run()` esce subito se il
  thread ha rinunciato, il caso *fallimento* è veloce — ma il caso *lento* resta bloccante, e la
  risposta andrebbe restituita subito lasciando che sia la coda a far attendere il client.
- **`get_buffer` dichiara un errore invece della fine dello stream.** Controlla
  `media_in == nullptr || cancel_read` **prima** di svuotare la coda, quindi appena la sorgente
  termina scarta i buffer residui e ritorna `MHD_CONTENT_READER_END_WITH_ERROR`. Il client riceve
  una risposta abortita e un browser la segnala come "file danneggiato".
- `media_in` punta a una `FormatContext` **locale** di `streaming_core`: fra il `return` di quella
  funzione e `media_in = nullptr` in `streaming_thread` il puntatore è dangling, e `get_buffer` lo
  dereferenzia per leggere `cancel_read`.
