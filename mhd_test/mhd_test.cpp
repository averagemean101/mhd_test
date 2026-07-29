#include "stdafx.h"

constexpr auto BUFSIZE = 4096;

// The Rai relinker answers 403 without a browser User-Agent. It is also the one HTTP option that
// ffio_copy_url_options() forwards to nested connections, so setting it once here covers the
// playlist and every segment fetch -- unlike tls_verify, which needs the io_open hook in avpp.
constexpr auto BROWSER_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
                            "(KHTML, like Gecko) Chrome/124.0 Safari/537.36";

// Sources that are actually reachable. The Mediaset playlists that used to be here were saved in
// 2021 and their CDN no longer resolves in DNS, so they are gone rather than listed as broken.
//
// The Rai entries use output=7 with forceUserAgent, which makes the relinker answer a plain 302 to
// the HLS playlist. FFmpeg follows redirects, so this needs no XML handling -- output=64 instead
// returns a <Mediapolis> document with the URL buried in CDATA, which would have to be parsed.
struct Channel {
    const char* slug; // matched against /live/<...>, lowercased
    const char* url;
};

constexpr Channel CHANNELS[] = {
    { "tv8",  "https://www.mytivu.it/Application/Channels/TV8.php" },
    { "rai1", "https://mediapolis.rai.it/relinker/relinkerServlet.htm"
              "?cont=2606803&output=7&forceUserAgent=raiplayappletv" },
    { "rai2", "https://mediapolis.rai.it/relinker/relinkerServlet.htm"
              "?cont=308718&output=7&forceUserAgent=raiplayappletv" },
};

// Returns the channel whose slug appears in the request path, or nullptr.
static const Channel* find_channel(const std::string& lowered_path)
{
    for (const auto& channel : CHANNELS) {
        if (lowered_path.find(channel.slug) != std::string::npos) {
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
        if (media_in != nullptr) {
            media_in->cancel_read = true;
        }
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

    void Run()
    {
        int timeout = 100;
        ++live_generators();
        live_thread = std::thread{ &BaseGenerator::streaming_thread, this }; // thread starts immediately
        // Same missing condition as in get_buffer: once the thread has given up, media_in can never
        // be set, so there is nothing left to wait for. Without this the handler burned the full
        // 10 s before returning a response that could only fail.
        while (this->media_in == nullptr && !streaming_done && --timeout > 0) {
            std::this_thread::sleep_for(100ms);
        }
        if (timeout <= 0) {
            cout << "Started BaseGenerator" << (timeout <= 0 ? " (BUT WITH TIMEOUT!)." : ".") << "\n";
        }
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
        int ret = 0;
        int retries_counter = 0;
        bool normal_exit = false;

        while (retries_counter++ < max_retries) 
        {
            try
            {
                ret = streaming_core();
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

            this->media_in = nullptr;

            if (normal_exit || stop_retrying) {
                break;
            }

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

        // Publish that nobody will ever set media_in again. Whoever waits on it -- Run() and
        // get_buffer -- has to be able to tell "not ready yet" from "never going to be ready".
        streaming_done = true;
    }

    virtual int streaming_core() = 0; // Pure virtual: must be implemented in derived class.

protected:
    bool stop_retrying = false;
    avpp::FormatContext* media_in = nullptr;
    // Read by the MHD thread and by Run(), written by the streaming thread: has to be atomic.
    std::atomic<bool> streaming_done{ false };
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
        // Not an impossible state, despite what this used to claim: MHD asks for data as soon as the
        // response is queued, and the source can still be opening. What matters is giving up when
        // the streaming thread is done, because then media_in will never be set. Without that exit
        // this loop spun forever -- measured at 26520 iterations over 427 s, with the client
        // receiving neither a byte nor an error, which is what left the browser on a spinner.
        while (media_in == nullptr && !streaming_done) {
            std::this_thread::sleep_for(10ms);
        }
        if (media_in == nullptr) {
            return static_cast<ssize_t>(MHD_CONTENT_READER_END_WITH_ERROR);
        }

        while (stream_queue.size() <= 0 && (media_in != nullptr && !media_in->cancel_read)) {
            std::this_thread::sleep_for(10ms);
        }

        if (media_in == nullptr || media_in->cancel_read) {
            return static_cast<ssize_t>(MHD_CONTENT_READER_END_WITH_ERROR);
        }

        auto opt = stream_queue.pop();
        if (opt) {
            auto vec = *opt;
            auto lenght = vec->size();
            auto size = FFMIN(lenght, max);
            memcpy(buf, vec->data(), size); // ottimizzare con un move?
            delete vec, vec = nullptr;

            return static_cast<ssize_t>(size);
        }

        cout << "+++++++++++++++++++++++++++++++++ return END_OF_STREAM\n";
        return static_cast<ssize_t>(MHD_CONTENT_READER_END_OF_STREAM);
    }

    int write_cb(const uint8_t* buf, int size)
    {
        auto v = new std::vector<uint8_t>(size);
        memcpy(v->data(), buf, size);
        stream_queue.push(v);
        return size;
    }

    virtual int streaming_core() override
    {
        auto wr_cb = [&](const uint8_t* a, int b)->int { return this->write_cb(a, b); };
        ::av_log_set_level(AV_LOG_WARNING);

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
        input.add_filter_graph(input.id_audio, { 
            { "aresample", "44100" },
            { "aformat", "fltp" },
            { "asetnsamples", "1024" },
            { "asettb", avpp::FLICKS_TIMESCALE_STR }
        });
        this->media_in = &input;

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
    LivePage live;
    FilesPage files;
    WebServer server(8080);
    server.addController(&live);
    server.addController(&files);
    cout << "Server started on port " << server.get_port() << ".\n";
    server.start();
    cout << "Server stopped.\n";

    return 0;
}
