#pragma once
#pragma warning(disable : 26812) // remove unscoped enums warning

#define _CRT_SECURE_NO_WARNINGS

extern "C"
{
	#include <libavcodec/avcodec.h>
	#include <libavformat/avformat.h>
	#include <libavfilter/avfilter.h>
	#include <libavfilter/buffersink.h>
	#include <libavfilter/buffersrc.h>
	#include <libavdevice/avdevice.h>
	#include <libavutil/opt.h>
	#include <libavutil/error.h>
}

#include <iostream>
#include <chrono>
#include <fstream>
#include <vector>
#include <thread>
#include <string>
#include <iomanip>
#include <algorithm>
#include <functional>
#include <map>
#include <queue>
#include <mutex>
#include <atomic>
#include <filesystem>

// utils.h needs <sstream> and ThreadSafeQueue.h needs <optional>, but neither
// includes them. Under /std:c++latest they arrived transitively through other
// standard headers; at /std:c++17 they do not. They are listed here, and not
// fixed in common/, because that is a separate repository shared with another
// project.
#include <sstream>
#include <optional>

#include <microhttpd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h> // _get_pgmptr, to resolve paths against the executable's own directory
#include <conio.h>
#include <utils.h>
#include <avpp.h>
#include <ThreadSafeQueue.h>
#include <HttpServer.h>
