#include "ffmpeg_capture.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>

#include "bmusb/bmusb.h"
#include "ffmpeg_raii.h"
#include "ffmpeg_util.h"
#include "flags.h"
#include "image_input.h"

#define FRAME_SIZE (8 << 20)  // 8 MB.

using namespace std;
using namespace std::chrono;
using namespace bmusb;

namespace {

steady_clock::time_point compute_frame_start(int64_t frame_pts, int64_t pts_origin, const AVRational &video_timebase, const steady_clock::time_point &origin, double rate)
{
	const duration<double> pts((frame_pts - pts_origin) * double(video_timebase.num) / double(video_timebase.den));
	return origin + duration_cast<steady_clock::duration>(pts / rate);
}

bool changed_since(const std::string &pathname, const timespec &ts)
{
	if (ts.tv_sec < 0) {
		return false;
	}
	struct stat buf;
	if (stat(pathname.c_str(), &buf) != 0) {
		fprintf(stderr, "%s: Couldn't check for new version, leaving the old in place.\n", pathname.c_str());
		return false;
	}
	return (buf.st_mtim.tv_sec != ts.tv_sec || buf.st_mtim.tv_nsec != ts.tv_nsec);
}

}  // namespace

FFmpegCapture::FFmpegCapture(const string &filename, unsigned width, unsigned height)
	: filename(filename), width(width), height(height)
{
	// Not really used for anything.
	description = "Video: " + filename;

	avformat_network_init();  // In case someone wants this.
}

FFmpegCapture::~FFmpegCapture()
{
	if (has_dequeue_callbacks) {
		dequeue_cleanup_callback();
	}
}

void FFmpegCapture::configure_card()
{
	if (video_frame_allocator == nullptr) {
		owned_video_frame_allocator.reset(new MallocFrameAllocator(FRAME_SIZE, NUM_QUEUED_VIDEO_FRAMES));
		set_video_frame_allocator(owned_video_frame_allocator.get());
	}
	if (audio_frame_allocator == nullptr) {
		owned_audio_frame_allocator.reset(new MallocFrameAllocator(65536, NUM_QUEUED_AUDIO_FRAMES));
		set_audio_frame_allocator(owned_audio_frame_allocator.get());
	}
}

void FFmpegCapture::start_bm_capture()
{
	if (running) {
		return;
	}
	running = true;
	producer_thread_should_quit.unquit();
	producer_thread = thread(&FFmpegCapture::producer_thread_func, this);
}

void FFmpegCapture::stop_dequeue_thread()
{
	if (!running) {
		return;
	}
	running = false;
	producer_thread_should_quit.quit();
	producer_thread.join();
}

std::map<uint32_t, VideoMode> FFmpegCapture::get_available_video_modes() const
{
	// Note: This will never really be shown in the UI.
	VideoMode mode;

	char buf[256];
	snprintf(buf, sizeof(buf), "%ux%u", width, height);
	mode.name = buf;
	
	mode.autodetect = false;
	mode.width = width;
	mode.height = height;
	mode.frame_rate_num = 60;
	mode.frame_rate_den = 1;
	mode.interlaced = false;

	return {{ 0, mode }};
}

void FFmpegCapture::producer_thread_func()
{
	char thread_name[16];
	snprintf(thread_name, sizeof(thread_name), "FFmpeg_C_%d", card_index);
	pthread_setname_np(pthread_self(), thread_name);

	while (!producer_thread_should_quit.should_quit()) {
		string pathname = search_for_file(filename);
		if (filename.empty()) {
			fprintf(stderr, "%s not found, sleeping one second and trying again...\n", filename.c_str());
			send_disconnected_frame();
			producer_thread_should_quit.sleep_for(seconds(1));
			continue;
		}
		if (!play_video(pathname)) {
			// Error.
			fprintf(stderr, "Error when playing %s, sleeping one second and trying again...\n", pathname.c_str());
			send_disconnected_frame();
			producer_thread_should_quit.sleep_for(seconds(1));
			continue;
		}

		// Probably just EOF, will exit the loop above on next test.
	}

	if (has_dequeue_callbacks) {
                dequeue_cleanup_callback();
		has_dequeue_callbacks = false;
        }
}

void FFmpegCapture::send_disconnected_frame()
{
	// Send an empty frame to signal that we have no signal anymore.
	FrameAllocator::Frame video_frame = video_frame_allocator->alloc_frame();
	if (video_frame.data) {
		VideoFormat video_format;
		video_format.width = width;
		video_format.height = height;
		video_format.stride = width * 4;
		video_format.frame_rate_nom = 60;
		video_format.frame_rate_den = 1;
		video_format.is_connected = false;

		video_frame.len = width * height * 4;
		memset(video_frame.data, 0, video_frame.len);

		frame_callback(timecode++,
			video_frame, /*video_offset=*/0, video_format,
			FrameAllocator::Frame(), /*audio_offset=*/0, AudioFormat());
	}
}

bool FFmpegCapture::play_video(const string &pathname)
{
	// Note: Call before open, not after; otherwise, there's a race.
	// (There is now, too, but it tips the correct way. We could use fstat()
	// if we had the file descriptor.)
	timespec last_modified;
	struct stat buf;
	if (stat(pathname.c_str(), &buf) != 0) {
		// Probably some sort of protocol, so can't stat.
		last_modified.tv_sec = -1;
	} else {
		last_modified = buf.st_mtim;
	}

	auto format_ctx = avformat_open_input_unique(pathname.c_str(), nullptr, nullptr);
	if (format_ctx == nullptr) {
		fprintf(stderr, "%s: Error opening file\n", pathname.c_str());
		return false;
	}

	if (avformat_find_stream_info(format_ctx.get(), nullptr) < 0) {
		fprintf(stderr, "%s: Error finding stream info\n", pathname.c_str());
		return false;
	}

	int video_stream_index = find_stream_index(format_ctx.get(), AVMEDIA_TYPE_VIDEO);
	if (video_stream_index == -1) {
		fprintf(stderr, "%s: No video stream found\n", pathname.c_str());
		return false;
	}

	const AVCodecParameters *codecpar = format_ctx->streams[video_stream_index]->codecpar;
	AVRational video_timebase = format_ctx->streams[video_stream_index]->time_base;
	AVCodecContextWithDeleter codec_ctx = avcodec_alloc_context3_unique(nullptr);
	if (avcodec_parameters_to_context(codec_ctx.get(), codecpar) < 0) {
		fprintf(stderr, "%s: Cannot fill codec parameters\n", pathname.c_str());
		return false;
	}
	AVCodec *codec = avcodec_find_decoder(codecpar->codec_id);
	if (codec == nullptr) {
		fprintf(stderr, "%s: Cannot find decoder\n", pathname.c_str());
		return false;
	}
	if (avcodec_open2(codec_ctx.get(), codec, nullptr) < 0) {
		fprintf(stderr, "%s: Cannot open decoder\n", pathname.c_str());
		return false;
	}
	unique_ptr<AVCodecContext, decltype(avcodec_close)*> codec_ctx_cleanup(
		codec_ctx.get(), avcodec_close);

	internal_rewind();
	double rate = 1.0;

	unique_ptr<SwsContext, decltype(sws_freeContext)*> sws_ctx(nullptr, sws_freeContext);
	int sws_last_width = -1, sws_last_height = -1;

	// Main loop.
	while (!producer_thread_should_quit.should_quit()) {
		// Process any queued commands from other threads.
		vector<QueuedCommand> commands;
		{
			lock_guard<mutex> lock(queue_mu);
			swap(commands, command_queue);
		}
		for (const QueuedCommand &cmd : commands) {
			switch (cmd.command) {
			case QueuedCommand::REWIND:
				if (av_seek_frame(format_ctx.get(), /*stream_index=*/-1, /*timestamp=*/0, /*flags=*/0) < 0) {
					fprintf(stderr, "%s: Rewind failed, stopping play.\n", pathname.c_str());
				}
				// If the file has changed since last time, return to get it reloaded.
				// Note that depending on how you move the file into place, you might
				// end up corrupting the one you're already playing, so this path
				// might not trigger.
				if (changed_since(pathname, last_modified)) {
					return true;
				}
				internal_rewind();
				break;

			case QueuedCommand::CHANGE_RATE:
				start = next_frame_start;
				pts_origin = last_pts;
				rate = cmd.new_rate;
				break;
			}
		}

		// Read packets until we have a frame or there are none left.
		int frame_finished = 0;
		AVFrameWithDeleter frame = av_frame_alloc_unique();
		bool eof = false;
		do {
			AVPacket pkt;
			unique_ptr<AVPacket, decltype(av_packet_unref)*> pkt_cleanup(
				&pkt, av_packet_unref);
			av_init_packet(&pkt);
			pkt.data = nullptr;
			pkt.size = 0;
			if (av_read_frame(format_ctx.get(), &pkt) == 0) {
				if (pkt.stream_index != video_stream_index) {
					// Ignore audio for now.
					continue;
				}
				if (avcodec_send_packet(codec_ctx.get(), &pkt) < 0) {
					fprintf(stderr, "%s: Cannot send packet to codec.\n", pathname.c_str());
					return false;
				}
			} else {
				eof = true;  // Or error, but ignore that for the time being.
			}

			int err = avcodec_receive_frame(codec_ctx.get(), frame.get());
			if (err == 0) {
				frame_finished = true;
				break;
			} else if (err != AVERROR(EAGAIN)) {
				fprintf(stderr, "%s: Cannot receive frame from codec.\n", pathname.c_str());
				return false;
			}
		} while (!eof);

		if (!frame_finished) {
			// EOF. Loop back to the start if we can.
			if (av_seek_frame(format_ctx.get(), /*stream_index=*/-1, /*timestamp=*/0, /*flags=*/0) < 0) {
				fprintf(stderr, "%s: Rewind failed, not looping.\n", pathname.c_str());
				return true;
			}
			// If the file has changed since last time, return to get it reloaded.
			// Note that depending on how you move the file into place, you might
			// end up corrupting the one you're already playing, so this path
			// might not trigger.
			if (changed_since(pathname, last_modified)) {
				return true;
			}
			internal_rewind();
			continue;
		}

		if (sws_ctx == nullptr || sws_last_width != frame->width || sws_last_height != frame->height) {
			sws_ctx.reset(
				sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
					width, height, AV_PIX_FMT_BGRA,
					SWS_BICUBIC, nullptr, nullptr, nullptr));
			sws_last_width = frame->width;
			sws_last_height = frame->height;
		}
		if (sws_ctx == nullptr) {
			fprintf(stderr, "%s: Could not create scaler context\n", pathname.c_str());
			return false;
		}

		VideoFormat video_format;
		video_format.width = width;
		video_format.height = height;
		video_format.stride = width * 4;
		video_format.frame_rate_nom = video_timebase.den;
		video_format.frame_rate_den = av_frame_get_pkt_duration(frame.get()) * video_timebase.num;
		if (video_format.frame_rate_nom == 0 || video_format.frame_rate_den == 0) {
			// Invalid frame rate.
			video_format.frame_rate_nom = 60;
			video_format.frame_rate_den = 1;
		}
		video_format.has_signal = true;
		video_format.is_connected = true;

		next_frame_start = compute_frame_start(frame->pts, pts_origin, video_timebase, start, rate);
		last_pts = frame->pts;

		FrameAllocator::Frame video_frame = video_frame_allocator->alloc_frame();
		if (video_frame.data != nullptr) {
			uint8_t *pic_data[4] = { video_frame.data, nullptr, nullptr, nullptr };
			int linesizes[4] = { int(video_format.stride), 0, 0, 0 };
			sws_scale(sws_ctx.get(), frame->data, frame->linesize, 0, frame->height, pic_data, linesizes);
			video_frame.len = video_format.stride * height;
			video_frame.received_timestamp = next_frame_start;
		}

		FrameAllocator::Frame audio_frame;
		AudioFormat audio_format;
                audio_format.bits_per_sample = 32;
                audio_format.num_channels = 8;

		producer_thread_should_quit.sleep_until(next_frame_start);
		frame_callback(timecode++,
			video_frame, 0, video_format,
			audio_frame, 0, audio_format);
	}
	return true;
}

void FFmpegCapture::internal_rewind()
{				
	pts_origin = last_pts = 0;
	start = next_frame_start = steady_clock::now();
}
