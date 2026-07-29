#include "stdafx.h"

constexpr auto BUFSIZE = 4096;

using namespace std;
using namespace http;

////////////////////////////////////////////////////////////////////////////////////////////////////////// STREAM GENERATORS

class BaseGenerator {
public:
    BaseGenerator()
    {
        //cout << "BaseGenerator constructor.\n";
    }

    virtual ~BaseGenerator()
    {
        stop_retrying = true;
        if (media_in != nullptr) {
            media_in->cancel_read = true;           
            //cout << " media_in->cancel_read = true\n";
        }
        if (live_thread.joinable()) {
            live_thread.join();
            //cout << "Stopped BaseGenerator.\n";
        }
        //cout << "BaseGenerator destructor.\n";
    }

    void Run()
    {
        int timeout = 100;
        live_thread = std::thread{ &BaseGenerator::streaming_thread, this }; // thread starts immediately
        while (this->media_in == nullptr && --timeout > 0) {
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
            else {
                cout << "Attempt #" << retries_counter << " to generate stream failed. Retrying...\n";
                this_thread::sleep_for(500ms);
            }
        }
    }

    virtual int streaming_core() = 0; // Pure virtual: must be implemented in derived class.

protected:
    bool stop_retrying = false;
    avpp::FormatContext* media_in = nullptr;
    std::function<void(avpp::FormatContext& media)> prog_cb = [&](avpp::FormatContext& f) { progress_cb(f); };

private:
    int max_retries = 3;
    std::thread live_thread;
};

class MP4Generator : public BaseGenerator {
public:
    MP4Generator(const char* url)
    {
        this->url = url;
    }

    virtual ~MP4Generator() noexcept override { } // created just for noexcept
    
    // Returns a byte count, or one of MHD's two sentinels. The signature has to be signed to be
    // able to say that. Returning them from a size_t function does happen to work, because the
    // macros are SIZE_MAX and SIZE_MAX-1, the same bit patterns the ssize_t conversion turns back
    // into -1 and -2 -- but nothing in the code says so, it is a C4245 the project only misses by
    // building at /W3, and the caller's "> 0" test reads like a guard while never firing for them.
    ssize_t get_buffer(char* buf, size_t max)
    {
        while (media_in == nullptr) {
            cout << "Should never happen!\n";
            std::this_thread::sleep_for(10ms);
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
        std::string live_url;
        url = str_tolower(url);
        auto wr_cb = [&](const uint8_t* a, int b)->int { return this->write_cb(a, b); };
        ::av_log_set_level(AV_LOG_WARNING);

        if (str_contains_by_val(url, "rai1")) {
            live_url = "https://mediapolis.rai.it/relinker/relinkerServlet.htm?cont=2606803";
        }
        else if (str_contains_by_val(url, "italia1")) {
            live_url = "d:\\downloads\\tv\\Italia1.m3u8";
        }
        else if (str_contains_by_val(url, "focus")) {
            live_url = "d:\\downloads\\tv\\Focus.m3u8";
        }
        else if (str_contains_by_val(url, "tv8")) {
            live_url = "https://www.mytivu.it/Application/Channels/TV8.php";
        }
        else
        {
            cout << "ERROR: Live mp4 stream '" << url << "' non found! Defaulting to focus...\n";
            live_url = "d:\\downloads\\tv\\Focus.m3u8";
        }

        auto input = avpp::format_open_input(live_url, prog_cb, "", {
            { "protocol_whitelist" , "file,https,tcp,tls,crypto" },
            // Required only behind the corporate TLS-inspecting proxy, which
            // breaks certificate revocation checking for every https source.
            // This turns off peer verification on ALL connections of this
            // stream: see the README section on TLS inspection before removing
            // the condition or making it unconditional.
            { "insecure_tls" , "1" }
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

class DASHGenerator : public BaseGenerator {
public:
    virtual int streaming_core() override
    {
        std::string live_url;
        std::string output_file;
        ::av_log_set_level(AV_LOG_WARNING);
        
        live_url = "d:\\downloads\\tv\\focus.m3u8"s;
        filesystem::current_path("d:\\downloads\\www\\focus.dash\\data\\");
        output_file = "focus.mpd";
        
        auto input = avpp::format_open_input(live_url, prog_cb, "", { { "protocol_whitelist" , "file,https,tcp,tls,crypto" } });
        input.dump_format();
        input.open_best_streams();
        this->media_in = &input;
        
        auto output = avpp::format_open_output(output_file, "dash", input, {
            { "use_timeline",    "1" },
            { "use_template",    "1" },
            { "window_size",     "5" },
            { "remove_at_exit",  "1" },
            { "adaptation_sets", "id=0,streams=v id=1,streams=a"}
        });
        output.new_stream(AVMEDIA_TYPE_VIDEO, AV_CODEC_ID_H264, 2048, {
            { "g",            "120" },
            { "bf",           "1" },
            { "profile",      "main" },
            { "preset",       "ultrafast" },
            { "keyint_min",   "120" },
            { "sc_threshold", "0" },
            { "b_strategy",   "0" } // -flags +cgop ?
        });
        output.new_stream(AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC, 96);
                
        output.create();        

        return 0;
    }
};

class HLSGenerator : public BaseGenerator {
public:
    virtual int streaming_core() override
    {
        std::string live_url;
        std::string output_file;
        ::av_log_set_level(AV_LOG_WARNING);

        //live_url = "d:\\downloads\\tv\\focus.m3u8"s;
        live_url = "https://mediapolis.rai.it/relinker/relinkerServlet.htm?cont=2606803"s;
        filesystem::current_path("d:\\downloads\\www\\focus.hls\\data\\");
        output_file = "focus.m3u8";

        auto input = avpp::format_open_input(live_url, prog_cb, "", { 
            { "protocol_whitelist", "file,https,tcp,tls,crypto" }, 
            { "hw_accel",           "dxva2" }
        });
        input.dump_format();
        input.open_best_streams();
        //input.add_filter_graph(input.id_video, { 
        //    { "drawtext", "fontfile='c:\\Windows\\Fonts\\gilsanub.ttf':fontcolor=white:fontsize=26:"
        //                  "text='::max::':x=14:y=14:shadowcolor=black:shadowx=1:shadowy=1:alpha=0.7" } });
            //{ "drawtext", "fontfile='c:\\Windows\\Fonts\\verdana.ttf':fontcolor=black:fontsize=28:box=1:boxborderw=2:"
            //    "text='[max]':x=843:y=497:alpha=0.6" }});
        this->media_in = &input;

        auto output = avpp::format_open_output(output_file, "hls", input, {
            { "hls_time",                "6" },
          //{ "hls_wrap",                "40" }, // rimossa dalla v.4.4
            { "hls_delete_threshold",    "1" },
            { "hls_flags",               "delete_segments" },
            { "start_number",            "0" }
        });
        output.new_stream(AVMEDIA_TYPE_VIDEO, "h264_amf", 2048, {
            { "g",            "120" },
            { "keyint_min",   "120" },
            { "bf",           "1" },
            { "profile",      "main" },
            { "preset",       "ultrafast" }, // con hw_accel il preset è unico
            { "sc_threshold", "0" }, // con hw_accel non è configurabile
            { "b_strategy",   "0" }  // con hw_accel non ci sono b-frames
        });
        output.new_stream(AVMEDIA_TYPE_AUDIO, AV_CODEC_ID_AAC, 96);

        output.create();

        return 0;
    }
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
        MP4Generator* mp4 = new MP4Generator(url);

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
        delete mp4, mp4 = nullptr;
        cout << "MP4 DELETED 1\n";
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
        const std::string root = "d:\\downloads\\www\\"s;
        filesystem::path url_to_parse{ url };
        filesystem::path url_abs_path{ root };
        auto rel_path = std::string(&url[1]);
        url_abs_path /= rel_path;

        if (url_to_parse.has_filename()) { // send file
            ifstream* file_to_send = new ifstream(url_abs_path, ios::in | ios::binary);
            if (file_to_send->is_open()) {
                callback_data = { data_generator_file, data_generator_file_free, 2*BUFSIZE, file_to_send };
                headers = { { MHD_HTTP_HEADER_TRANSFER_ENCODING, "chunked" } };
            }
            else {
                delete file_to_send, file_to_send = nullptr;
                response << get_html_page("\tCannot open file \"" + string(url) + "\"\n");
                status_code = MHD_HTTP_NOT_FOUND;
            }
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
    // Disabled: HLSGenerator is hardwired to the Rai relinker, which answers 403 from the Akamai
    // edge. It retried three times at startup and held Run() for its full 10 s timeout, delaying
    // the web server by that much to produce nothing. Recovering Rai needs a browser User-Agent,
    // &output=64 and parsing the XML it returns in CDATA -- see the README. Live streaming is
    // unaffected: it goes through LivePage, on demand, per request.
    //DASHGenerator generator;
    //HLSGenerator generator;
    //generator.Run();

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
