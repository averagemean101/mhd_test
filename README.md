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
| `GET /live/{rai1,rai2,rai3,italia1,tv8,20,focus}` | MP4 frammentato in streaming, `Content-Type: video/mp4` |
| `GET /live/<altro>` | `404` |
| `GET /<path>` | file server, oppure l'elenco della cartella se `<path>` è una directory senza `index.html` |
| fuori dal root | `403` |

I canali stanno nella tabella `CHANNELS` di [mhd_test.cpp](mhd_test/mhd_test.cpp): aggiungerne uno è una riga lì più un `<button>` in `www/index.html`. `LivePage` risolve lo slug e passa la URL già risolta al generator, che quindi non ripete la ricerca.

Lo slug è confrontato **per intero** con il pezzo di path dopo `/live/`, ignorando uno slash finale.
Prima era una ricerca di sottostringa, che con uno slug corto come `20` avrebbe risposto per
qualunque path contenente quelle due cifre.

Verificato sulle rotte, non solo sul canale: ogni slug risolve `200` sia nella forma nuda sia con lo
slash finale, e le quasi-collisioni restano `404` — `italia` che è prefisso di `italia1`, `2` che è
sottostringa sia di `rai2` sia di `20`, `tv` prefisso di `tv8`, più `20x`, `canale20` e `rete4`. Per
`focus`, aggiunto dopo, la stessa coppia più `focu` (prefisso) e `focus2` (estensione): `404`
entrambi.

Il root del file server è la cartella **`www/` accanto all'eseguibile**, non la working directory:
il programma si lancia tipicamente come `.\build\Release\mhd_test.exe` dalla radice del repo, e un
path relativo alla CWD cercherebbe nel posto sbagliato. `www/` è versionata qui e CMake la copia
accanto all'exe a ogni build, quindi **dopo aver editato la pagina serve un rebuild**.

## Stato delle sorgenti (01/08/2026)

Sette canali, tutti verificati end-to-end attraverso l'applicazione: ognuno consegna stream
decodificabile, e le due rotte di controllo `/live/rete4` e `/live/canale20` rispondono `404`.

Misure prese **fuori dalla VPN aziendale**, a differenza dei rapporti contenuto/wall clock delle
sezioni sui difetti più sotto, che sono tutti dietro proxy: non sono confrontabili. Catture da 20 s,
zero errori di decodifica e zero `MUXER: write failed` su tutti e sette.

| canale | sorgente | consegnato | contenuto / wall clock |
|---|---|---|---|
| `rai1` | relinker Rai, `cont=2606803` | 1920x1080 @5689 kbps | 1,95 |
| `rai2` | relinker Rai, `cont=308718` | 1920x1080 @5689 kbps | 1,95 |
| `rai3` | relinker Rai, `cont=308709` | 1920x1080 @5689 kbps | 1,96 |
| `italia1` | `live02-seg.msf.cdn.mediaset.net/live/ch-i1/i1-clr.isml/index.m3u8` | 1920x1080**@50p**, 8269 kbps | 1,25 |
| `tv8` | `mytivu.it/Application/Channels/TV8.php` | 854x480 @3196 kbps, tetto della sua ladder | 1,99 |
| `20` | `.../live/ch-lb/lb-clr.isml/index.m3u8` | 1920x1080**@50p**, 8269 kbps | 1,05 |
| `focus` | `.../live/ch-fu/fu-clr.isml/index.m3u8` | 1920x1080, 8269 kbps | 1,05 |

Note sulle sorgenti: la `.php` di `tv8` conia un token Akamai nuovo a ogni chiamata, quindi va
invocata quella e **non** la URL che restituisce; i codici canale Mediaset sono `i1`, `lb` e `fu`
(vedi «Trovare il codice»); i tre Rai passano dal relinker con `output=7`.

**Le risoluzioni sono quelle del 01/08/2026**, quando `open_best_streams()` ha smesso di prendere la
prima variante del master playlist per prendere la più grande — prima `focus` e `italia1` uscivano a
1024x576. Il dettaglio è in [common/README.md](../common/README.md), §2; qui conta la conseguenza
misurata: il margine sul tempo reale **si è assottigliato** dove il rung è cresciuto di più, da 1,15
a 1,05 su `focus`. Resta sopra 1,0, ma è il numero da guardare per primo quando si proverà il
multi-client.

`focus` porta lo stesso difetto di timestamp degli altri Mediaset, scalato al suo frame rate:
`dts = pts + 3600` a 90 kHz, cioè 40 ms, un intervallo di frame a 25 fps (sui 50p il delta è 1800).
È stata anche la prima prova sul campo del filtro del log: oltre mille occorrenze ridotte a sette
righe, e nove in tutto sullo stderr della sessione.

### Mediaset: l'host giusto, e come è stato trovato

Le playlist Mediaset del 2021 puntavano a `liveN-mediaset-it.akamaized.net`, che **non esiste più**:
`NXDOMAIN` sia dal resolver locale sia da `8.8.8.8` e `1.1.1.1`, quindi non è un effetto della VPN.
Anche `liveN.msf.cdn.mediaset.net`, che compare nelle liste IPTV community, è `NXDOMAIN`.

L'host vivo è `live02-seg.msf.cdn.mediaset.net` (esiste anche `live03-seg`, non `live01-seg`), con lo
schema Unified Streaming `/live/ch-<id>/<id>-clr.isml/index.m3u8`. Il suffisso `-clr` è la resa in
chiaro: le playlist non contengono `#EXT-X-KEY`, quindi non c'è niente da decifrare. Nessun `.mpd`
è raggiungibile su quell'host: le tre forme provate rispondono `451`.

### Trovare il codice `<id>` di un canale Mediaset

Un `200` non identifica il canale. Sondando l'host, undici codici su diciassette rispondono con un
master playlist valido, e i manifest sono **indistinguibili**: stessa versione di Unified Streaming,
stesso gruppo audio, stesso ladder. L'unico campo che varia è il nome del file, che ripete il codice
già scritto nella URL. Un codice sbagliato darebbe quindi un canale sbagliato che funziona
perfettamente — il modo più subdolo di sbagliare.

La mappatura sta fuori: si ricava dalle liste IPTV community, cercando il codice e leggendo il nome
dell'`#EXTINF` che lo precede. Tre liste indipendenti concordano
([Tundrak](https://raw.githubusercontent.com/Tundrak/IPTV-Italia/refs/heads/main/iptvita.m3u),
[Free-TV](https://raw.githubusercontent.com/Free-TV/IPTV/master/playlists/playlist_italy.m3u8),
[peppenamir](https://raw.githubusercontent.com/peppenamir/iptv_italia/main/lista.m3u)), e **il
controllo è già in casa**: tutte e tre danno `i1` per Italia 1 e `lb` per «20», che sono i due codici
che stiamo già usando e che abbiamo verificato per conto nostro. Una lista che sbagliasse quelli non
meriterebbe fiducia sugli altri.

I codici così ottenuti: `r4` Rete 4, `c5` Canale 5, `i1` Italia 1, `lb` 20, `ki` Iris, `ts` Twenty
Seven, `ka` La 5, `b6` Cine34, **`fu` Focus**, `lt` Top Crime, `kb` Boing, `la` Cartoonito, `i2`
Italia 2, `kf` TGcom24, `kq` Mediaset Extra.

**Il riassunto generato da un motore di ricerca su queste stesse liste era sbagliato**: dava `fu` per
RTL 102.5 e `b6` per Focus. Le liste, lette direttamente, dicono `fu` Focus e `b6` Cine34. Vale la
pena andare alla fonte anche quando la risposta sintetizzata sembra precisa.

L'API di Mediaset (`api-ott-prod-fe.mediaset.net/PROD/play/feed/allChannelHome/v2.0`), che darebbe la
mappatura ufficiale, risponde `403` sia nuda sia con User-Agent, `Referer` e `Origin` da browser.

### I timestamp rotti di quei TS, e i due frame su tre che costavano

Questi canali uscivano allora a 1024x576**@50p** — la variante che si prendeva prima della selezione
per risoluzione, non un limite della sorgente — ma alla prima misura arrivavano a **16,6 fps**: 60 ms esatti tra
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

### Quel `Invalid timestamps` continua a scorrere, ed è giusto così

La riparazione è in `send_pkt()`, cioè **in scrittura**; il messaggio lo emette libavformat quando
**legge** il pacchetto dalla sorgente, prima che il nostro codice lo tocchi. Dice cosa manda
Mediaset, non cosa perdiamo noi, e resterebbe anche se il muxer fosse perfetto. Ogni pacchetto
compare due volte, una per `[mpegts]` (il demuxer del segmento, `stream=0`) e una per `[hls]` (quello
che lo incapsula, `stream=1`): stessi `pts`/`dts`/`size`, stessa diagnostica a due livelli. Il delta
è sempre `1800` a 90 kHz, cioè 20 ms, un intervallo di frame a 50 fps.

**Che non si stia perdendo nulla si verifica altrove**: dall'assenza di `MUXER: write failed on
stream N, frame lost` e dal frame rate consegnato, non dal silenzio di questa riga.

Restava il volume. Il filtro installato in `main()` limita **per tipo di messaggio**: prime 3
occorrenze per intero, poi una riga che dichiara la soppressione, poi un ri-annuncio a ogni potenza
di dieci, e a `Server stopped.` un riepilogo `Nx <messaggio>` di tutto ciò che è stato trattenuto —
una sorgente che ripete la stessa diagnostica 16 000 volte è un fatto su quella sorgente, e il
conteggio è l'unica traccia che ne resta.

Il tipo è la **format string** (`"Invalid timestamps stream=%d, pts=%s, ..."`): i valori che variano
stanno negli argomenti, quindi contare per format string collassa il diluvio senza toccare ciò che
viene detto poche volte. Due alternative più economiche non funzionano, ed è il motivo per cui il
filtro esiste: abbassare la soglia a `AV_LOG_ERROR` zittisce anche i warning che compaiono una volta
sola, che sono quelli che contano; `AV_LOG_SKIP_REPEATED` unisce solo righe **identiche**, e queste
differiscono in ogni timestamp.

Il callback fa da sé il filtro di severità (mascherando i bit di colore di `AV_LOG_C()`): riceve il
messaggio, non è documentato che lo riceva già filtrato, e contare righe che non verrebbero comunque
stampate falserebbe i totali. Scrive su `cerr`, dove scrive `av_log_default_callback`: su `cout` le
note si staccherebbero dai messaggi che annotano appena uno dei due stream viene rediretto. Ed è
thread safe, perché libav logga dai suoi thread di decodifica e qui anche da un thread di streaming
**per client**.

Nella stessa modifica `av_log_set_level` si è spostata da `streaming_core()` a `main()`: è
un'impostazione di processo e veniva rieseguita a ogni richiesta, da un thread di streaming.

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

`rai3` (`cont=308709`) è stato aggiunto con questo schema e verificato: 1920x1080 come `rai1`, quindi
è il canale live e non un asset VOD. Attenzione appunto agli ID indovinati: `cont=2606805` risponde
con un `podcastcdn/.../2606805.mp4`, cioè **VOD**.

### Sorgenti rimosse

| ex canale | perché |
|---|---|
| `Cielo` (mytivu) | l'endpoint esiste ma risponde `302` senza `Location` utile |

**`focus` è rientrato il 29/07/2026** ed è ora nella tabella dei canali. Stava qui per lo stesso
motivo di `italia1` — playlist Mediaset salvata nel 2021, il cui CDN non risolve più in DNS — e si
recupera nello stesso modo, sull'host vivo con il codice `fu`.

Con lui e con `Cielo` erano spariti `DASHGenerator` e `HLSGenerator`, che erano già disattivati,
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

## Quando la sorgente muore a metà: il fallimento che si traveste da successo

Sintomo osservato il 29/07/2026 su `focus`, **fuori dalla VPN**: ogni tanto l'immagine si ferma e non
riparte più; ricliccare il canale la fa ripartire subito. Log del server al momento dello stop:

```
[tls @ ...] Creating security context failed (0x80090304)
[hls @ ...] Failed to reload playlist 1
  DEMUX: real EOF.
  MUXER: trailer written (2276).
```

La catena, e dove si perde l'informazione:

1. `0x80090304` è `SEC_E_INTERNAL_ERROR` — «The Local Security Authority cannot be contacted»,
   risolto interrogando il sistema. È **Schannel sulla macchina locale** che non riesce a creare il
   contesto TLS: con la CDN non si è nemmeno parlato. Da non confondere con `0x80092012` della
   sezione sul proxy: quello è la revoca, questo è un fallimento locale e transitorio.
2. `hls` non può rinfrescare la media playlist, quindi resta senza segmenti.
3. **Il demuxer traduce quel fallimento in EOF**, indistinguibile da una sorgente finita davvero. È
   qui che l'informazione si perde: da questo punto in poi tutti si comportano correttamente su una
   premessa falsa.
4. Il server ci crede, scrive il trailer e chiude la risposta pulita: `200`, corpo completo.
5. `<video>` riceve un file finito e si ferma. Non ritenta perché non ha niente a cui reagire.

Tre ragioni **indipendenti** per cui non si ripara da solo: `hls` non insiste sul reload fallito
(`seg_max_retry` copre i segmenti, non le playlist); il server si vieta il retry a byte già
consegnati, per la regola della sezione precedente; il client vede una fine normale.

### Il recupero sta nel client, perché è l'unico punto dove è legittimo

Una richiesta **nuova** produce una risposta nuova, quindi un `ftyp/moov` nuovo è corretto: è
esattamente ciò che fa ricliccare il canale a mano, e `www/index.html` ora lo fa da sé. Su `ended`
ri-sintonizza lo stesso canale fino a **5 tentativi** con backoff crescente, con un `?t=<timestamp>`
in coda alla URL — sulla stessa URL il browser può rigiocarsi la risposta appena finita invece di
chiederne una nuova. Il server non la vede: MHD separa la query dal path prima del router
(verificato, `/live/focus?t=123` → `200 video/mp4`).

Tre dettagli non ovvi:

- un **contatore di generazione** invalida un recupero già in coda se nel frattempo l'utente cambia
  canale o spegne, altrimenti un timer partito prima cambierebbe canale sotto le mani;
- il **budget si ricarica** dopo 5 s di riproduzione sana, altrimenti un canale guardato un'ora
  affronterebbe la prima vera interruzione con i tentativi spesi per un singhiozzo di un'ora prima;
- si ascolta **solo `ended`, non `error`**, ed è deliberato. Assegnare un nuovo `src` aborta la
  richiesta in volo e il browser può segnalare quell'abort come errore sull'elemento: recuperare da
  `error` farebbe sì che un cambio canale programmi un «recupero» che taglia il canale appena
  sintonizzato. `ended` non scatta su un load abortito, quindi il trigger più stretto non ha bisogno
  di euristiche. Prezzo dichiarato: una risposta che si spezza **senza** chiudersi pulita resta neve
  finché l'utente non interviene.

**Non verificato a runtime.** Su questa macchina non c'è nessun runtime JavaScript e non c'è un
browser pilotabile: il codice è stato controllato rileggendolo, non eseguendolo. La conferma sul
campo è nel log del server — un secondo `MUXER: header written.` entro pochi secondi dal
`DEMUX: real EOF.`, senza che nessuno abbia cliccato.

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

---

## Prossimi passi — annotati, non implementati

Tre idee raccolte il 29/07/2026, da riprendere sull'altro PC. Nessuna è iniziata: qui c'è il
contesto che serve per decidere, non un progetto.

### 1. Un file di configurazione

Il candidato che rende la cosa necessaria è **`insecure_tls`**. Oggi vale `"1"` incondizionatamente
in [mhd_test.cpp](mhd_test/mhd_test.cpp), quindi la verifica del certificato è disattivata su *tutte*
le connessioni di ogni stream, sempre. Serve solo dietro il proxy aziendale con TLS inspection (vedi
la sezione «HTTPS dietro un proxy con TLS inspection»): fuori da quella rete il programma è meno
sicuro di quanto sembri leggendolo, e il commento accanto all'opzione parla di una condizione che
**non esiste** nel codice. È il motivo principale per fare questo passo, non un dettaglio di comodo.

Altri candidati, tutti oggi cablati: la porta (`8080`), lo `User-Agent` da browser, `max_retries` e
il backoff del retry, `http_persistent` e `seg_max_retry`, il sottoalbero servito (`www`), e
plausibilmente la tabella `CHANNELS`, che è l'unica ragione per cui aggiungere un canale richiede un
rebuild.

Un candidato è salito di rango: **`http_persistent`**, oggi `"0"` per tutti. È stato messo per il
proxy aziendale, che uccide le connessioni persistenti, ma il suo effetto è aprire una connessione
TLS **nuova a ogni reload di playlist e a ogni segmento** — cioè un handshake ogni pochi secondi, e
ogni handshake un'occasione per il `0x80090304` che interrompe lo stream (vedi la sezione sopra).
Fuori dalla VPN è quindi tutto costo e nessun beneficio, esattamente come `insecure_tls`: stesso
schema, due impostazioni che servono solo dentro quella rete e che fuori peggiorano le cose. Cablarle
al valore opposto non risolverebbe, sposterebbe solo il problema all'altro ambiente.

Ci va anche il **log level di libav** — `AV_LOG_WARNING` fisso in `main()` — e con lui
`FFMPEG_LOG_LINES_PER_KIND`, la soglia del filtro descritto sopra. Sono i due parametri che si
vogliono cambiare proprio quando ricompilare è scomodo: per indagare una sorgente che si comporta
male si alza a `AV_LOG_VERBOSE`, o si toglie il limite per vedere tutte le righe, e appena finito si
torna indietro. Nota di merito rispetto agli altri candidati: sono le uniche due impostazioni che
**non** cambiano cosa fa il programma, solo cosa racconta, quindi si possono rendere configurabili
senza alcun rischio sul comportamento.

Da decidere prima di scrivere codice: **formato** (un `.ini` piatto si scrive senza dipendenze, un
JSON richiede una libreria), **posizione** (accanto all'eseguibile, come già fa `www/`), e cosa
succede **se il file manca** — la risposta giusta è probabilmente «default sicuri», cioè
`insecure_tls` a `0`, così l'impostazione pericolosa va chiesta esplicitamente.

### 2. Trasformarlo in un servizio

Oggi `main()` resta appeso a `getc(stdin)` e la chiusura pulita è un `\n`. Come servizio quel
modello non esiste: non c'è stdin.

Il vincolo vero è che il progetto deve buildare anche su Linux, quindi sono **due** integrazioni
diverse — `ServiceMain`/SCM su Windows, unit `systemd` con `Type=notify` su Linux — e va isolata
dietro un'astrazione sottile, non sparsa nel codice. Servono comunque, in entrambi i casi: un log su
file o su journal invece che su `std::cout`, e un percorso di shutdown che non passi da stdin (quindi
la separazione `listen()`/`wait_and_stop()` già fatta in `HttpServer.h` è il pezzo giusto su cui
appoggiarsi). Da valutare se non basti, molto più a buon mercato, un task pianificato o un
`systemd` unit banale che lancia l'eseguibile così com'è.

### 3. Due antennine a V sopra il televisore

Puramente estetico, tutto in [www/index.html](www/index.html). Vanno sopra `.cabinet`, con
`transform: rotate()` sui due bracci e `transform-origin` alla base, e devono stare **fuori** dal
flusso che dimensiona lo schermo, altrimenti spostano il 16:9. Servono `pointer-events: none` come
già per gli altri ornamenti, e un `z-index` sotto al mobile se si vuole l'effetto «piantate dietro».
Attenzione al layout responsive: la media query a schermo stretto mette la lista canali in riga, e
un'antenna che sborda va gestita lì.

### 4. Verifica con client multipli, server su un PC e client su un altro

**Non è mai stato provato.** Tutti i test fatti finora sono sequenziali e su loopback: l'unica
concorrenza osservata è stata l'accavallamento transitorio di due generator durante un teardown
(`MP4 DELETED 1, generators still streaming: 1`).

Il fatto architetturale da cui partire: `LivePage::createResponse` fa `new MP4Generator(...)` **a ogni
richiesta**, e ogni generator apre il proprio input FFmpeg. Non c'è nessun fan-out, quindi *N* client
sullo stesso canale scaricano *N* volte la stessa sorgente. Banda a monte e thread crescono col numero
di client, non di canali — e i thread si sommano: `MHD_USE_THREAD_PER_CONNECTION` ne mette uno per
connessione, più uno di streaming per generator, più quelli di libav (`get_enc_dec_threads()`, oggi
limitata a 16). Se emergesse un limite, la risposta giusta è probabilmente condividere un generator
fra i client dello stesso canale, che però cambia il modello: il fMP4 va servito a partire da un
`ftyp`/`moov` per ciascuno.

Cosa misurare, con gli strumenti già scritti: il rapporto **contenuto consegnato / wall clock** per
ogni client (deve restare ≥ 1,0, come nella sezione sulle varianti scartate) e il conteggio
`generators still streaming` alla disconnessione, che deve tornare a zero.

Tre rischi che la concorrenza amplifica, tutti già noti: il race su `cancel_read` (più teardown
simultanei, più probabilità di incrociarlo), la coda dell'header che cresce senza limite se uno
stream non riceve mai pacchetti (§5 del README di `common`), e il rate limiting sull'endpoint mytivu,
che conia un token per chiamata — con più client simultanei è il primo candidato a rispondere male.

**Sull'ambiente, verificato il 29/07/2026 su questo PC:**

- il server ascolta su `0.0.0.0:8080`, quindi è raggiungibile da rete **senza modifiche al codice**;
- `www/index.html` usa URL relative (`/live/...`), quindi funziona da un altro host così com'è;
- **ma** esistono otto regole firewall inbound **di Block** per `mhd_test.exe`, attive sui profili
  Private e Public — nate dai prompt di Windows a ogni nuovo eseguibile. Con quelle in piedi il test
  cross-machine falliva per una ragione che non ha niente a che vedere col codice. Da ispezionare
  prima di iniziare:

  ```powershell
  Get-NetFirewallApplicationFilter | Where-Object { $_.Program -like '*mhd_test*' } |
      ForEach-Object { Get-NetFirewallRule -AssociatedNetFirewallApplicationFilter $_ } |
      Select-Object DisplayName, Direction, Action, Enabled, Profile
  ```

- attenzione all'indirizzo su cui puntare il client: questa macchina è multi-homed (vedi «DNS
  ballerino»), e fra le interfacce ci sono adattatori VMware, un adattatore Cato e diversi
  link-local `169.254.*`. Va usato l'IP dell'interfaccia Ethernet reale, non il primo che si trova.
