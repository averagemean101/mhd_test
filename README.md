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

Se la porta è occupata l'eseguibile lo dice e ritorna 1, senza annunciare un avvio che non c'è
stato. Con stdin chiusa o rediretta dal nulla, `getc` vedrebbe EOF subito: in quel caso il server
**resta su** e va terminato con un segnale. Per pilotarlo da script basta tenere stdin aperta e
mandare `\n`, che è anche l'unico modo per far flushare `std::cout` quando è rediretto su file.

Poi <http://127.0.0.1:8080/>, che serve la pagina con il link per avviare lo stream.

| rotta | risposta |
|---|---|
| `GET /` | `www/index.html`: lista canali a sinistra, televisore a destra |
| `GET /live/{rai1,rai2,italia1,tv8,20}` | MP4 frammentato in streaming, `Content-Type: video/mp4` |
| `GET /live/<altro>` | `404` |
| `GET /<path>` | file server, oppure l'elenco della cartella se `<path>` è una directory senza `index.html` |
| fuori dal root | `403` |

I canali stanno nella tabella `CHANNELS` di [mhd_test.cpp](mhd_test/mhd_test.cpp): aggiungerne uno è una riga lì più un `<button>` in `www/index.html`. `LivePage` risolve lo slug e passa la URL già risolta al generator, che quindi non ripete la ricerca.

Lo slug è confrontato **per intero** con il pezzo di path dopo `/live/`. Prima era una ricerca di
sottostringa, che con lo slug `20` avrebbe risposto per qualunque path contenente quelle due cifre:
`/live/canale20` restituisce `404`, come deve.

Il root del file server è la cartella **`www/` accanto all'eseguibile**, non la working directory:
il programma si lancia tipicamente come `.\build\Release\mhd_test.exe` dalla radice del repo, e un
path relativo alla CWD cercherebbe nel posto sbagliato. `www/` è versionata qui e CMake la copia
accanto all'exe a ogni build, quindi **dopo aver editato la pagina serve un rebuild**.

## Stato delle sorgenti (29/07/2026)

Cinque canali, tutti verificati end-to-end attraverso l'applicazione: ognuno consegna stream
decodificabile (18/18/1,8/7,8/2,5 MB in 12 s), e le due rotte di controllo `/live/rete4` e
`/live/canale20` rispondono `404`.

| canale | sorgente | note |
|---|---|---|
| `rai1` | relinker Rai, `cont=2606803` | vedi sotto |
| `rai2` | relinker Rai, `cont=308718` | vedi sotto |
| `italia1` | `live02-seg.msf.cdn.mediaset.net/live/ch-i1/i1-clr.isml/index.m3u8` | vedi «Mediaset» e il difetto sui 50 fps |
| `tv8` | `mytivu.it/Application/Channels/TV8.php` | la `.php` conia un token Akamai nuovo a ogni chiamata: va invocata quella, **non** la URL che restituisce |
| `20` | `.../live/ch-lb/lb-clr.isml/index.m3u8` | idem; il codice canale di «20» è `lb` |

### Mediaset: l'host giusto, e come è stato trovato

Le playlist Mediaset del 2021 puntavano a `liveN-mediaset-it.akamaized.net`, che **non esiste più**:
`NXDOMAIN` sia dal resolver locale sia da `8.8.8.8` e `1.1.1.1`, quindi non è un effetto della VPN.
Anche `liveN.msf.cdn.mediaset.net`, che compare nelle liste IPTV community, è `NXDOMAIN`.

L'host vivo è `live02-seg.msf.cdn.mediaset.net` (esiste anche `live03-seg`, non `live01-seg`), con lo
schema Unified Streaming `/live/ch-<id>/<id>-clr.isml/index.m3u8`. Il suffisso `-clr` è la resa in
chiaro: le playlist non contengono `#EXT-X-KEY`, quindi non c'è niente da decifrare. Nessun `.mpd`
è raggiungibile su quell'host: le tre forme provate rispondono `451`.

### I timestamp rotti di quei TS, e i due frame su tre che costavano

Questi canali sono 1024x576**@50p**, ma alla prima misura uscivano a **16,6 fps**: 60 ms esatti tra
un frame e l'altro in 521 intervalli su 571, e nessun B-frame nell'output (`B=0 I=27 P=544`) contro
`B=375 I=9 P=192` della sorgente. Spariva esattamente un tipo di immagine, cioè 2 frame su 3.

La causa è nella sorgente: i segmenti TS Mediaset danno a ogni B-frame `dts = pts + 1800` a 90 kHz,
cioè un DTS **successivo** al PTS. È invalido — nessun frame può essere mostrato prima di essere
decodificato — e `mpegts` lo segnala (16 741 righe `Invalid timestamps` in una sessione, **tutte**
con delta 1800). Il muxer `mp4` poi rifiuta il pacchetto con `EINVAL`, e `avpp::send_pkt()`
scartava il valore di ritorno: il frame se ne andava in silenzio.

La riparazione, in [avpp.cpp](../common/avpp.cpp), è quella che applica la catena di muxing di
ffmpeg stessa, e sono **due** passi — il primo da solo non basta:

1. se `dts > pts`, schiaccia `dts = pts`;
2. se il `dts` così ottenuto non è **strettamente** maggiore dell'ultimo scritto, spingilo avanti di
   un tick. Serve perché in questi stream il pts di un B-frame coincide col dts del pacchetto
   precedente, quindi il passo 1 produce un duplicato e `mp4` lo rifiuta a sua volta
   (`non monotonically increasing dts ... 14112235 >= 14112235`). Alla scala dei flicks un tick è
   1,4 ns, quindi l'istante di visualizzazione non si muove.

Applicando solo il passo 1 si passava da 16,6 a 31,5 fps — metà dei B-frame recuperati. Con
entrambi: **49,98 fps su un taglio pulito, con zero errori di decodifica**, e `r_frame_rate` letto
correttamente come `50/1`. Contatori a valle della correzione: `letti=1448, entrati=1448,
scritti=1448, falliti=0`. `tv8` e `rai1`, che sono a 25 fps, restano identici.

Il percorso è stato trovato strumentando temporaneamente il loop di `avpp` con un contatore per
stream; la strumentazione è stata rimossa, ma `send_pkt()` ora **dichiara** una scrittura fallita
(`MUXER: write failed on stream N, frame lost`, prime 8 per contesto). Prima l'unica traccia era il
messaggio di FFmpeg, che non nomina lo stream e si perde in un log che il demuxer sta già riempiendo.

### Le varianti non usate non si scaricano più

Lo stesso contatore ha fatto emergere un secondo difetto, indipendente: su `italia1` arrivavano 1448
pacchetti video utili e **7235 da stream che poi buttavamo**; su `rai1` 1011 contro 16256. Sono le
altre varianti di bitrate del master HLS. `av_find_best_stream()` ne sceglie una, ma
`open_best_streams()` non metteva `AVDISCARD_ALL` sulle altre, e il demuxer `hls` tiene viva una
playlist finché **uno qualsiasi** dei suoi stream sta sotto `AVDISCARD_ALL`: le scaricava tutte.

Non era solo banda buttata. Su questa rete dietro proxy lo stream non stava al passo col tempo reale:

| `italia1`, cattura da 45 s | contenuto consegnato | wall clock | rapporto |
|---|---|---|---|
| prima | 26,9 s | 45,0 s | **0,60** |
| dopo | 57,6 s | 49,0 s | **1,18** |

Con `discard_unselected_streams()` il server ora annuncia cosa ha scartato — `19 unselected stream(s)
discarded, 2 kept` sui canali Rai, 5 sui Mediaset, 7 su `tv8` — e tutti e cinque i canali stanno
sopra 1,0, cioè avanti al tempo reale. Il frame rate resta quello giusto (48,3 fps misurati sui
Mediaset, con la coda troncata dal taglio a spiegare il resto).

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
| `focus` | playlist Mediaset salvata nel 2021, il cui CDN non risolve più in DNS. `italia1` era nella stessa condizione ed è stato **recuperato** sul nuovo host: vedi sopra |
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

## Come è coordinato il producer/consumer

Il thread di streaming produce, `get_buffer` consuma sul thread di MHD, e il coordinamento passa
per tre atomiche invece che per il puntatore `media_in`:

| stato | significato per `get_buffer` |
|---|---|
| coda non vuota | consegna un elemento |
| coda vuota, `streaming_done == false` | **attende**: la sorgente può ancora aprirsi o essere in retry |
| coda vuota, `streaming_done == true` | fine: `END_WITH_ERROR` se `streaming_failed`, altrimenti `END_OF_STREAM` |

`get_buffer` non legge più `media_in`, che è privato e sotto mutex: lo pubblica `PublishedInput`,
un guard RAII dichiarato **dopo** la `FormatContext` a cui punta, così l'ordine inverso di
distruzione lo ritira prima che l'oggetto muoia. `request_cancel()` prende lo stesso lock, quindi
non può mai toccare una `FormatContext` in distruzione.

`Run()` non attende: la risposta viene committata subito e l'attesa la fa il consumatore. Da qui
segue una regola non ovvia — **un retry è ammesso solo finché il client non ha visto nulla**. Dopo
il primo byte consegnato, riavviare emetterebbe un secondo `ftyp/moov` a metà stream, che un player
legge come file corrotto: `output_handed_over` blocca il retry, e finché è falso
`discard_pending_output()` butta ciò che il tentativo fallito aveva accodato.

## Difetti noti, non corretti

- **Race residuo su `cancel_read`.** `request_cancel()` scrive `FormatContext::cancel_read` mentre
  l'interrupt callback di FFmpeg lo legge sul thread di streaming: è un `bool` semplice
  (`avpp.h:165`), quindi formalmente una data race. Non è stato reso `std::atomic<bool>` perché
  renderebbe `FormatContext` non copiabile **né movibile**, e `format_open_input()` la restituisce
  per valore: servirebbe darle semantiche di move esplicite.

  *Rettifica (29/07/2026):* una versione precedente di questa voce motivava la scelta anche con
  «un tipo condiviso con il progetto `v`». È falso e verificato tale: `v` non contiene nessun
  riferimento ad `avpp`, include da `common` solo `utils.h`. La ragione tecnica resta, quella sul
  progetto `v` no.
- Il file server non manda `Content-Type` per nulla che non sia `.html`: le altre estensioni
  arrivano senza, e il browser tira a indovinare.
