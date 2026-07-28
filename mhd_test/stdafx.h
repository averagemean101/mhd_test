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
#include <filesystem>

#include <microhttpd.h>
#include <signal.h>
#include <stdio.h>
#include <conio.h>
#include <utils.h>
#include <avpp.h>
#include <ThreadSafeQueue.h>
#include <HttpServer.h>
