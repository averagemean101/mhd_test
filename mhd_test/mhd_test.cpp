#include "stdafx.h"

constexpr auto BUFSIZE = 4096;

// The Rai relinker answers 403 without a browser User-Agent. It is also the one HTTP option that
// ffio_copy_url_options() forwards to nested connections, so setting it once here covers the
// playlist and every segment fetch -- unlike tls_verify, which needs the io_open hook in avpp.
constexpr auto BROWSER_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                            "(KHTML, like Gecko) Chrome/124.0 Safari/537.36";

// Sources that are actually reachable, listed in Italian channel-number order. Two families of
// them need a note:
//
// - The Rai entries use output=7 with forceUserAgent, which makes the relinker answer a plain 302 to
//   the HLS playlist. FFmpeg follows redirects, so this needs no XML handling -- output=64 instead
//   returns a <Mediapolis> document with the URL buried in CDATA, which would have to be parsed.
// - The Mediaset entries are served from live02-seg.msf.cdn.mediaset.net, not from the
//   liveN-mediaset-it.akamaized.net hosts of the playlists saved here in 2021: those names have no
//   DNS record any more, which two public resolvers confirm independently of the local one. The
//   "-clr" rendition is the one to ask for; its media playlists carry no #EXT-X-KEY, so nothing in
//   the path has to be decrypted.
struct Channel {
    const char* slug; // the path component after /live/, lowercased
    const char* url;
};

constexpr Channel CHANNELS[] = {
    { "rai1",    "https://mediapolis.rai.it/relinker/relinkerServlet.htm"
                 "?cont=2606803&output=7&forceUserAgent=raiplayappletv" },
    { "rai2",    "https://mediapolis.rai.it/relinker/relinkerServlet.htm"
                 "?cont=308718&output=7&forceUserAgent=raiplayappletv" },
    { "rai3",    "https://mediapolis.rai.it/relinker/relinkerServlet.htm"
                 "?cont=308709&output=7&forceUserAgent=raiplayappletv" },
    { "italia1", "https://live02-seg.msf.cdn.mediaset.net/live/ch-i1/i1-clr.isml/index.m3u8" },
    { "tv8",     "https://www.mytivu.it/Application/Channels/TV8.php" },
    { "20",      "https://live02-seg.msf.cdn.mediaset.net/live/ch-lb/lb-clr.isml/index.m3u8" },
    { "focus",   "https://live02-seg.msf.cdn.mediaset.net/live/ch-fu/fu-clr.isml/index.m3u8" },
};

// Returns the channel addressed by the request path, or nullptr.
//
// The name is compared whole rather than searched for: a substring match would let the two-digit
// slug "20" answer for any path that merely contains those digits.
static const Channel* find_channel(const std::string& lowered_path)
{
    const std::string prefix = "/live/";
    if (lowered_path.rfind(prefix, 0) != 0) {
        return nullptr;
    }

    std::string name = lowered_path.substr(prefix.size());
    while (!name.empty() && name.back() == '/') { // tolerate "/live/tv8/"
        name.pop_back();
    }

    for (const auto& channel : CHANNELS) {
        if (name == channel.slug) {
            return &channel;
        }
    }
    return nullptr;
}

// Served files live here, resolved from the executable's own directory rather than the working
// directory: the program is normally launched as .\build\Release\mhd_test.exe from the repository
// root, so anything CWD-relative would look in the wrong place.
constexpr auto WWW_SUBDIR = "www";

using namespace std;
using namespace http;

// The one platform-specific call in this file. _get_pgmptr is the MSVC CRT's copy of the executable
// path, used in preference to GetModuleFileName only to keep <windows.h> -- and its min/max macros,
// in a file that uses FFMIN -- out of the build. It is narrow, so a non-ASCII install path would be
// mangled; not worth handling for a test tool.
static filesystem::path exe_directory()
{
    char* pgm = nullptr;
    if (_get_pgmptr(&pgm) != 0 || pgm == nullptr) {
        return filesystem::current_path(); // nothing better to offer
    }
    return filesystem::path{ pgm }.parent_path();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////// FFMPEG LOG

// FFmpeg's diagnostics are worth keeping -- the Mediaset timestamp defect was found through them --
// but a source with a systematic defect repeats the same one for every packet: one session produced
// 16741 identical "Invalid timestamps" lines, which bury everything else, including the messages
// that only appear once and matter most.
//
// So the limit is per message *kind*, not per severity: dropping AV_LOG_WARNING would silence the
// one-off warnings too. The format string is the kind -- the timestamps that vary live in the
// arguments -- so counting by it collapses a flood into one entry while leaving anything said only
// a few times untouched.
//
// AV_LOG_SKIP_REPEATED does not do this: it only collapses lines that come out identical, and these
// differ in every timestamp.
constexpr int FFMPEG_LOG_LINES_PER_KIND = 3;

static mutex g_ffmpeg_log_mutex;
static map<string, long long> g_ffmpeg_log_counts;

// A message is re-announced when its count reaches a power of ten, so a long run stays visible
// without flooding: 10, 100, 1000 ... and nothing in between.
static bool is_power_of_ten(long long n)
{
    while (n >= 10 && n % 10 == 0) {
        n /= 10;
    }
    return n == 1;
}

// The format string carries its own trailing newline and would break the lines we wrap it in.
static string one_line(const string& fmt)
{
    const auto end = fmt.find_last_not_of(" \t\r\n");
    return end == string::npos ? fmt : fmt.substr(0, end + 1);
}

// Installed with av_log_set_callback, so it must be thread safe: libav* logs from its own decoding
// threads, and here also from one streaming thread per connected client.
//
// Everything this function prints goes to cerr, where av_log_default_callback writes: on cout the
// notices would drift away from the messages they annotate as soon as either stream is redirected.
static void ffmpeg_log_cb(void* avcl, int level, const char* fmt, va_list vl)
{
    // Filter by severity here instead of assuming the caller already did: a custom callback is
    // documented to receive the message, not to receive it pre-filtered, and counting lines that
    // would never be printed would make the tallies meaningless. The low byte is the level, the rest
    // can carry the colour bits of AV_LOG_C(); a negative level is AV_LOG_QUIET and is left alone.
    int severity = level;
    if (severity >= 0) {
        severity &= 0xff;
    }
    if (severity > ::av_log_get_level()) {
        return;
    }

    long long n = 0;
    {
        lock_guard<mutex> lock(g_ffmpeg_log_mutex);
        n = ++g_ffmpeg_log_counts[fmt ? fmt : "(no format string)"];
    }

    if (n <= FFMPEG_LOG_LINES_PER_KIND) {
        ::av_log_default_callback(avcl, level, fmt, vl);
        if (n == FFMPEG_LOG_LINES_PER_KIND) {
            cerr << "  LOG: further occurrences of the message above are counted, not printed.\n";
        }
        return;
    }

    if (is_power_of_ten(n)) {
        cerr << "  LOG: " << n << " occurrences so far: " << one_line(fmt ? fmt : "") << "\n";
    }
}

// What the rate limiter held back. A source that repeats one message ten thousand times is a fact
// about that source, and once the lines are gone the count is the only trace of it left.
static void dump_ffmpeg_log_tally()
{
    lock_guard<mutex> lock(g_ffmpeg_log_mutex);

    bool any = false;
    for (const auto& entry : g_ffmpeg_log_counts) {
        if (entry.second <= FFMPEG_LOG_LINES_PER_KIND) {
            continue;
        }
        if (!any) {
            cerr << "FFmpeg messages that were rate-limited:\n";
            any = true;
        }
        cerr << "  " << entry.second << "x " << one_line(entry.first) << "\n";
    }
}

////////////////////////////////////////////////////////////////////////////////////////////////////////// STREAM GENERATORS

class BaseGenerator {
public:
    BaseGenerator()
    {
        //cout << "BaseGenerator constructor.\n";
    }

    // Signals the streaming thread and waits for it. Idempotent.
    //
    // This MUST be called from the destructor of the most derived class. C++ destroys the derived
    // members before it runs ~BaseGenerator, so joining only here leaves the streaming thread
    // running for the whole window in between -- calling write_cb, which pushes into a
    // stream_queue that no longer exists. Measured: 143 buffers still queued and the muxer still
    // advancing at the moment MHD's free-callback deleted the object.
    void stop_streaming() noexcept
    {
        stop_retrying = true;
        request_cancel();
        if (live_thread.joinable()) {
            live_thread.join();
            --live_generators(); // only ever runs once: after the join the thread is no longer joinable
        }
    }

    virtual ~BaseGenerator()
    {
        // Backstop only. A derived class that forgets to call stop_streaming() still gets its
        // thread joined, but by then its own members are already gone -- so this is damage
        // control, not the mechanism.
        stop_streaming();
    }

    // Starts producing and returns immediately.
    //
    // It used to wait up to 100 x 100 ms for the source to open before returning, which held the
    // HTTP handler -- and so the response headers -- for as much as 10 s while the browser sat on a
    // spinner with nothing to show. Waiting is the consumer's job: get_buffer can tell "not ready
    // yet" from "never going to be ready", so the response can be committed straight away.
    void Run()
    {
        ++live_generators();
        live_thread = std::thread{ &BaseGenerator::streaming_thread, this }; // thread starts immediately
    }

    void progress_cb(avpp::FormatContext& src)
    {
        static char anim_frames[5] = "-\\|/";
        static int anim_pos = 0;
        auto c = anim_frames[anim_pos++ % 4];
        cout << "  " << c << " " << HH_mm_ss_ms(src.current_ms) << " " << c << "\r";
    }

    void streaming_thread()
    {
        int retries_counter = 0;
        bool normal_exit = false;

        while (retries_counter++ < max_retries)
        {
            try
            {
                (void)streaming_core();
                normal_exit = true;
            }
            catch (avpp::av_error& e)
            {
                cout << "av_error: code=" << e.err_code() << ", text=\"" << e.err_text() << "\"" << endl;
            }
            catch (exception& e)
            {
                cout << "exception: \"" << e.what() << "\"" << endl;
            }
            catch (...)
            {
                cout << "UNKNOWN EXCEPTION!" << endl;
            }

            // media_in is not cleared here any more: PublishedInput withdraws it inside
            // streaming_core, before the FormatContext it points at is destroyed. Doing it here left
            // the pointer dangling for the whole window in between.

            if (normal_exit || stop_retrying) {
                break;
            }

            // A retry can only ever be transparent while the client has seen nothing. Once bytes are
            // out, restarting would emit a second ftyp/moov in the middle of the stream, which a
            // player reads as a corrupt file -- so stop instead and let the consumer end cleanly.
            // This became reachable when Run() stopped waiting for the source: the response is now
            // committed before the first attempt has had a chance to fail.
            if (output_handed_over) {
                cout << "Attempt #" << retries_counter
                     << " failed after bytes were already sent: not restarting.\n";
                break;
            }

            // Nothing has left yet, so whatever the failed attempt queued must go: it would other-
            // wise sit in front of the next attempt's header.
            discard_pending_output();

            // Say "giving up" on the last attempt instead of "Retrying...", which used to be printed
            // even when no further attempt was coming.
            const bool last_attempt = (retries_counter >= max_retries);
            cout << "Attempt #" << retries_counter << " to generate stream failed"
                 << (last_attempt ? ", giving up.\n" : ". Retrying...\n");
            if (last_attempt) {
                break;
            }

            // Growing backoff. DNS for the CDN host fails in bursts on a multi-homed machine, and
            // a flat 500 ms x 3 gave the stream up after ~1.5 s -- far too short a fuse to ride out
            // a transient resolver failure.
            this_thread::sleep_for(500ms * retries_counter);
        }

        // Order matters: the outcome has to be visible before production is declared over, so that a
        // consumer which observes streaming_done can trust streaming_failed.
        streaming_failed = !normal_exit && !stop_retrying;
        streaming_done = true;
    }

    virtual int streaming_core() = 0; // Pure virtual: must be implemented in derived class.

    // Drops whatever a failed attempt produced but nobody consumed. Only called while
    // output_handed_over is false, so nothing observable is ever thrown away.
    virtual void discard_pending_output() {}

protected:
    // Publishes the input for the duration of one attempt. Use PublishedInput rather than calling
    // this directly: the pointer refers to a local of streaming_core and must be withdrawn before
    // that local dies.
    void publish_input(avpp::FormatContext* in) noexcept
    {
        std::lock_guard<std::mutex> lock{ media_mutex };
        media_in = in;
    }

    // Asks the input to stop reading. Under the same lock as publication, so it can never touch a
    // FormatContext that is already being destroyed.
    void request_cancel() noexcept
    {
        std::lock_guard<std::mutex> lock{ media_mutex };
        if (media_in != nullptr) {
            media_in->cancel_read = true;
        }
    }

    // RAII publication of the input. Declare it *after* the FormatContext it refers to: reverse
    // destruction order then withdraws the pointer before the object goes away. Without this,
    // media_in stayed set from the moment streaming_core returned until streaming_thread cleared it,
    // and anything reading it in between dereferenced a destroyed object.
    class PublishedInput {
    public:
        PublishedInput(BaseGenerator& owner, avpp::FormatContext& input) : owner(owner)
        {
            owner.publish_input(&input);
        }
        ~PublishedInput() { owner.publish_input(nullptr); }

        PublishedInput(const PublishedInput&) = delete;
        PublishedInput& operator=(const PublishedInput&) = delete;

    private:
        BaseGenerator& owner;
    };

    bool stop_retrying = false;
    // Written by the streaming thread, read by the MHD thread: both atomic.
    std::atomic<bool> streaming_done{ false };
    std::atomic<bool> streaming_failed{ false };
    // Set once the consumer has actually been handed bytes. From that point a retry is no longer
    // transparent, so it must not happen.
    std::atomic<bool> output_handed_over{ false };
    std::function<void(avpp::FormatContext& media)> prog_cb = [&](avpp::FormatContext& f) { progress_cb(f); };

public:
    // How many generators still have a live streaming thread. Incremented when Run() starts one,
    // decremented once the join in stop_streaming() has actually returned.
    static int active_generators() { return live_generators().load(); }

private:
    static std::atomic<int>& live_generators()
    {
        static std::atomic<int> n{ 0 };
        return n;
    }

    // Six attempts with a growing backoff spans roughly 8 s of retrying, against ~1.5 s before.
    int max_retries = 6;
    std::thread live_thread;

    // Guards media_in. The pointer is set and withdrawn on the streaming thread and read by whoever
    // cancels, so the lock is what makes "withdraw before destroying" actually mean something.
    std::mutex media_mutex;
    avpp::FormatContext* media_in = nullptr; // guarded by media_mutex
};

class MP4Generator : public BaseGenerator {
public:
    // Takes the resolved source URL, not the request path: LivePage has to look the channel up
    // anyway to answer 404, so doing it twice would be the only way to get them out of step.
    explicit MP4Generator(std::string source_url)
        : url(std::move(source_url))
    {
    }

    // Joins the streaming thread here, while this object's members are still alive. Leaving it to
    // ~BaseGenerator would let the thread write into an already-destroyed stream_queue.
    virtual ~MP4Generator() noexcept override { stop_streaming(); }
    
    // Returns a byte count, or one of MHD's two sentinels. The signature has to be signed to be
    // able to say that. Returning them from a size_t function does happen to work, because the
    // macros are SIZE_MAX and SIZE_MAX-1, the same bit patterns the ssize_t conversion turns back
    // into -1 and -2 -- but nothing in the code says so, it is a C4245 the project only misses by
    // building at /W3, and the caller's "> 0" test reads like a guard while never firing for them.
    ssize_t get_buffer(char* buf, size_t max)
    {
        // An empty queue is not an end: MHD asks for data as soon as the response is committed, the
        // source can take seconds to open, and a failed attempt is retried. The only real end is
        // "production is over", and even then only once what it left behind has been handed over.
        //
        // This no longer consults media_in at all, which is what removes the last read of a pointer
        // that could refer to a destroyed FormatContext.
        while (stream_queue.size() == 0 && !streaming_done) {
            std::this_thread::sleep_for(10ms);
        }

        auto opt = stream_queue.pop();
        if (opt) {
            auto vec = *opt;
            auto lenght = vec->size();
            auto size = FFMIN(lenght, max);
            memcpy(buf, vec->data(), size); // ottimizzare con un move?
            delete vec, vec = nullptr;

            output_handed_over = true; // from here on a retry would corrupt what the client has
            return static_cast<ssize_t>(size);
        }

        // The queue is drained and production is over. Which of MHD's two endings applies depends on
        // how production ended, and nothing is discarded to get here: the old code tested media_in
        // *before* draining, so a stream that ended normally threw away everything still queued and
        // reported an error -- which a browser renders as a corrupt file.
        return static_cast<ssize_t>(streaming_failed ? MHD_CONTENT_READER_END_WITH_ERROR
                                                     : MHD_CONTENT_READER_END_OF_STREAM);
    }

    int write_cb(const uint8_t* buf, int size)
    {
        auto v = new std::vector<uint8_t>(size);
        memcpy(v->data(), buf, size);
        stream_queue.push(v);
        return size;
    }

    // Called between a failed attempt and the next one, never once bytes have gone out.
    void discard_pending_output() override
    {
        while (auto opt = stream_queue.pop()) {
            auto vec = *opt;
            delete vec;
        }
    }

    virtual int streaming_core() override
    {
        auto wr_cb = [&](const uint8_t* a, int b)->int { return this->write_cb(a, b); };

        // Already resolved by LivePage. Not lowercased: these URLs carry case-sensitive tokens, and
        // the tv8 endpoint mints a fresh one on every call, so it is the URL to open -- never the one
        // it returns.
        const std::string& live_url = url;

        auto input = avpp::format_open_input(live_url, prog_cb, "", {
            { "protocol_whitelist" , "file,https,tcp,tls,crypto" },
            // The Rai relinker answers 403 without it, and it reaches the nested connections too.
            { "user_agent" , BROWSER_UA },
            // Required only behind the corporate TLS-inspecting proxy, which
            // breaks certificate revocation checking for every https source.
            // This turns off peer verification on ALL connections of this
            // stream: see the README section on TLS inspection before removing
            // the condition or making it unconditional.
            { "insecure_tls" , "1" },
            // Both are hls demuxer options, so avformat_open_input consumes them on the top-level
            // context -- no propagation problem, unlike the http protocol's reconnect_* family.
            //
            // The proxy kills persistent connections: every segment fetch first wasted an attempt on
            // a dead keepalive ("Writing encrypted data to socket failed") before opening a fresh
            // one. And seg_max_retry defaults to 0, meaning a segment that fails is abandoned on the
            // spot -- which is how two segments were lost to a transient DNS failure.
            { "http_persistent" , "0" },
            { "seg_max_retry"   , "2" }
        });
        input.dump_format();
        input.open_best_streams();
        if (input.id_audio >= 0) {
            // The source's own rate, named because loudnorm below drags the whole chain to 192 kHz
            // -- measured: the graph auto-inserts a resampler into it and its output stays there --
            // which is not even a valid AAC sample rate, so the way out has to say where to come
            // back to. Saying "the source's own" is what keeps this lossless: the sources differ,
            // Rai delivering 44,1 kHz and Mediaset 48 kHz, and the chain used to resample everything
            // to 44100 -- inert on one family, a pointless downsample on the other.
            const int audio_rate = input->streams[input.id_audio]->codecpar->sample_rate;

            input.add_filter_graph(input.id_audio, {
                // EBU R128 loudness normalisation, so the volume knob does not have to be touched
                // when changing channel. -16 LUFS rather than the broadcast -23: this ends up in a
                // browser page competing with everything else the machine is playing, not on a TV.
                //
                // It costs latency, and the figure was measured rather than assumed: ~2,7 s of added
                // pipeline lag, because in single-pass mode the filter looks ahead before emitting.
                // If that ever matters more than the levelling, `speechnorm` is the causal
                // alternative, measured at zero added lag. `dynaudnorm` is not an alternative here
                // at all: it emitted nothing whatsoever in 12 s of real-time input.
                { "loudnorm", "I=-16" },
                // One constraint filter rather than a resampler plus a format: the graph inserts
                // whatever conversion satisfies it. fltp because it is the only sample format the
                // AAC encoder accepts, stereo because a 5.1 source would otherwise reach the encoder
                // as 5.1 -- every source is stereo today, so that part is insurance, not a
                // conversion.
                { "aformat", avpp::FilterArgs(
                    "sample_fmts=fltp:sample_rates=%d:channel_layouts=stereo", audio_rate) },
                // AAC wants exactly 1024 samples per frame, and avpp does not call
                // av_buffersink_set_frame_size(), so the framing has to be asked for explicitly.
                // After the rate is settled, so the frames are 1024 samples of the final rate.
                { "asetnsamples", "1024" },
                // Back to the library's timescale: resampling leaves the output timebase at
                // 1/sample_rate, whoever inserted it.
                { "asettb", avpp::FLICKS_TIMESCALE_STR }
            });
        }
        // Declared after `input` on purpose: it is withdrawn before `input` is destroyed.
        PublishedInput published{ *this, input };

        auto output = avpp::format_open_output_to_buffer(wr_cb, BUFSIZE, "mp4", input, { { "movflags", "frag_keyframe+empty_moov" } });
        output.new_stream(AVMEDIA_TYPE_VIDEO);
        output.new_stream(AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC, 96);

        output.create();

        return 0;
    }

private:
    std::string url;
    ThreadsafeQueue<vector<uint8_t>*> stream_queue;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////// ROUTES HANDLERS

class LivePage : public DynamicController {
public:
    virtual bool validPath(const char* path, const char* method) override
    {       
        return (str_starts_with_by_val(path, "/live/") &&
                strcmp("GET", method) == 0);
    }

    virtual void createResponse(struct MHD_Connection* connection,
        const char* url, const char* method, const char* upload_data,
        size_t* upload_data_size, std::stringstream& response,
        Headers& headers, CallbackData& callback_data, int& status_code) override
    {
        // Reject unknown channels here, and not in streaming_core: by the time the generator runs,
        // the response has already been committed. streaming_thread would also retry three times
        // with a 500 ms wait, so the client would sit through 1.5 s and still receive a 200 with an
        // empty body.
        std::string requested_channel{ url }; // str_tolower takes a non-const reference
        str_tolower(requested_channel);
        const Channel* channel = find_channel(requested_channel);
        if (channel == nullptr) {
            cout << "GET: '" << url << "' -> 404 (no such channel)\n";
            response << get_html_page("\tNo such live channel \"" + string(url) + "\"\n");
            status_code = MHD_HTTP_NOT_FOUND;
            return;
        }

        MP4Generator* mp4 = new MP4Generator(channel->url);

        callback_data = { data_generator, data_generator_free, 2 * BUFSIZE, mp4 };
        headers = { { MHD_HTTP_HEADER_CONTENT_TYPE,      "video/mp4" },
                    { MHD_HTTP_HEADER_TRANSFER_ENCODING, "chunked"   } };

        mp4->Run();
        cout << "MP4 CREATED 1\n";
    }

    static ssize_t data_generator(void* cls, uint64_t pos, char* buf, size_t max)
    {
        MP4Generator* mp4 = reinterpret_cast<MP4Generator*>(cls);
        const ssize_t written_bytes = mp4->get_buffer(buf, max);

        // get_buffer already returns MHD's sentinels, so they pass straight through: collapsing
        // them onto END_OF_STREAM would throw away the distinction with END_WITH_ERROR. Only a
        // zero-length read is remapped, as before: MHD reads 0 as "no data yet" and would call
        // back immediately, spinning.
        return (written_bytes == 0)
            ? static_cast<ssize_t>(MHD_CONTENT_READER_END_OF_STREAM)
            : written_bytes;
    }

    static void data_generator_free(void* cls)
    {
        MP4Generator* mp4 = reinterpret_cast<MP4Generator*>(cls);
        delete mp4, mp4 = nullptr; // joins its streaming thread, so the count below is already updated
        cout << "MP4 DELETED 1, generators still streaming: "
             << BaseGenerator::active_generators() << "\n";
    }
};

class FilesPage : public DynamicController {
public:
    virtual bool validPath(const char* path, const char* method) override
    {
        return (strcmp(method, "GET") == 0); // accept all GETs
    }

    virtual void createResponse(struct MHD_Connection* connection,
        const char* url, const char* method, const char* upload_data,
        size_t* upload_data_size, std::stringstream& response,
        Headers& headers, CallbackData& callback_data, int& status_code) override
    {
        const filesystem::path root = (exe_directory() / WWW_SUBDIR).lexically_normal();
        filesystem::path url_to_parse{ url };
        auto rel_path = std::string(&url[1]);
        auto url_abs_path = (root / rel_path).lexically_normal();

        // Refuse anything that normalises its way out of the root. This matters more than it used
        // to: the root now sits inside the build tree, so an escape reaches the repository rather
        // than a downloads folder. MHD url-decodes before this point, so %2e%2e is already "..".
        // Comparing component-wise, not as strings, so that a sibling named "wwwroot" cannot pass
        // as a prefix of "www".
        const auto divergence = std::mismatch(root.begin(), root.end(),
                                              url_abs_path.begin(), url_abs_path.end());
        if (divergence.first != root.end()) {
            cout << "GET: '" << url << "' -> 403 (outside the served root)\n";
            response << get_html_page("\tPath outside the served root: \"" + string(url) + "\"\n");
            status_code = MHD_HTTP_FORBIDDEN;
            return;
        }

        if (url_to_parse.has_filename()) { // send file
            if (!send_file(url_abs_path, callback_data, headers)) {
                response << get_html_page("\tCannot open file \"" + string(url) + "\"\n");
                status_code = MHD_HTTP_NOT_FOUND;
            }
        }
        else if (send_file(url_abs_path / "index.html", callback_data, headers)) {
            // A directory with an index.html serves it instead of a listing, so that hitting
            // http://127.0.0.1:8080/ lands straight on the page.
        }
        else { // show folder content
            response << get_html_head();
            if (filesystem::exists(url_abs_path)) {
                if (rel_path.length() > 0) {
                    response << "\t<h1>- <a href='..'>" << "../" << "</a></h1>\n";
                }
                for (const auto& entry : filesystem::directory_iterator(url_abs_path)) {
                    auto is_folder = entry.is_directory();
                    response << "\t<h1>" << (is_folder ? "- " : "  ")
                             << "<a href='" << entry.path().filename().string() << (is_folder ? "/" : "") << "'>"
                             << entry.path().filename().string() << (is_folder ? "/" : "")
                             << "</a></h1>\n";
                }
            }
            else {
                response << "\tFolder not found. \"" << url << "\"\n";
                status_code = MHD_HTTP_NOT_FOUND;
            }
            response << get_html_tail();
        }
    }

private:
    // Opens the file and hands MHD the streaming callback. Returns false, having touched neither
    // callback_data nor headers, when the file is not there -- which is what lets the caller try an
    // index.html and fall back to a directory listing.
    static bool send_file(const filesystem::path& file, CallbackData& callback_data, Headers& headers)
    {
        if (!filesystem::is_regular_file(file)) {
            return false; // avoids allocating a stream just to discover it will not open
        }

        ifstream* file_to_send = new ifstream(file, ios::in | ios::binary);
        if (!file_to_send->is_open()) {
            delete file_to_send, file_to_send = nullptr;
            return false;
        }

        callback_data = { data_generator_file, data_generator_file_free, 2 * BUFSIZE, file_to_send };
        headers = { { MHD_HTTP_HEADER_TRANSFER_ENCODING, "chunked" } };

        // Without a Content-Type Firefox offers the page for download instead of rendering it. Only
        // HTML is mapped because that is all this folder serves: index.html carries its CSS inline
        // precisely so there is no second content type to get wrong.
        std::string ext = file.extension().string(); // str_tolower takes a non-const reference
        str_tolower(ext);
        if (ext == ".html" || ext == ".htm") {
            headers[MHD_HTTP_HEADER_CONTENT_TYPE] = "text/html; charset=utf-8";
        }
        return true;
    }

public:
    static ssize_t data_generator_file(void* cls, uint64_t pos, char* buf, size_t max)
    {
        ifstream* fts = reinterpret_cast<ifstream*>(cls);
        fts->read(buf, max);
        auto ret = fts->gcount() > 0 ? fts->gcount() : MHD_CONTENT_READER_END_OF_STREAM;
        return ret;
    }

    static void data_generator_file_free(void* cls)
    {
        ifstream* ftr = reinterpret_cast<ifstream*>(cls);
        delete ftr, ftr = nullptr;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////////// MAIN

int main(int argc, char** argv) 
{
    // Both settings are process-wide, so they belong here. av_log_set_level used to be called from
    // streaming_core(), i.e. once per connected client, on a thread that had no business owning it.
    ::av_log_set_level(AV_LOG_WARNING);
    ::av_log_set_callback(&ffmpeg_log_cb);

    LivePage live;
    FilesPage files;
    WebServer server(8080);
    server.addController(&live);
    server.addController(&files);

    // Announce the port only once it is actually bound. This used to print before start(), so an
    // occupied port produced a log that claimed a successful start and a process that served nothing.
    if (server.listen() != 0) {
        cout << "ERROR: cannot listen on port " << server.get_port() << " (already in use?).\n";
        return 1;
    }

    cout << "Server started on port " << server.get_port() << ".\n";
    server.wait_and_stop();
    cout << "Server stopped.\n";
    dump_ffmpeg_log_tally();

    return 0;
}
