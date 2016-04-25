#include "video_encoder.h"

#include <assert.h>

#include <string>

#include "defs.h"
#include "flags.h"
#include "httpd.h"
#include "timebase.h"
#include "quicksync_encoder.h"
#include "x264_encoder.h"

using namespace std;
using namespace movit;

namespace {

string generate_local_dump_filename(int frame)
{
	time_t now = time(NULL);
	tm now_tm;
	localtime_r(&now, &now_tm);

	char timestamp[256];
	strftime(timestamp, sizeof(timestamp), "%F-%T%z", &now_tm);

	// Use the frame number to disambiguate between two cuts starting
	// on the same second.
	char filename[256];
	snprintf(filename, sizeof(filename), "%s%s-f%02d%s",
		LOCAL_DUMP_PREFIX, timestamp, frame % 100, LOCAL_DUMP_SUFFIX);
	return filename;
}

}  // namespace

VideoEncoder::VideoEncoder(ResourcePool *resource_pool, QSurface *surface, const std::string &va_display, int width, int height, HTTPD *httpd)
	: resource_pool(resource_pool), surface(surface), va_display(va_display), width(width), height(height), httpd(httpd)
{
	oformat = av_guess_format(global_flags.stream_mux_name.c_str(), nullptr, nullptr);
	assert(oformat != nullptr);
	if (global_flags.stream_audio_codec_name.empty()) {
		stream_audio_encoder.reset(new AudioEncoder(AUDIO_OUTPUT_CODEC_NAME, DEFAULT_AUDIO_OUTPUT_BIT_RATE, oformat));
	} else {
		stream_audio_encoder.reset(new AudioEncoder(global_flags.stream_audio_codec_name, global_flags.stream_audio_codec_bitrate, oformat));
	}
	if (global_flags.x264_video_to_http) {
		x264_encoder.reset(new X264Encoder(oformat));
	}

	string filename = generate_local_dump_filename(/*frame=*/0);
	quicksync_encoder.reset(new QuickSyncEncoder(filename, resource_pool, surface, va_display, width, height, oformat, stream_audio_encoder.get(), x264_encoder.get()));

	open_output_stream();
	stream_audio_encoder->add_mux(stream_mux.get());
	quicksync_encoder->set_stream_mux(stream_mux.get());
	x264_encoder->set_mux(stream_mux.get());
}

VideoEncoder::~VideoEncoder()
{
	quicksync_encoder.reset(nullptr);
	close_output_stream();
}

void VideoEncoder::do_cut(int frame)
{
	string filename = generate_local_dump_filename(frame);
	printf("Starting new recording: %s\n", filename.c_str());
	quicksync_encoder->shutdown();
	quicksync_encoder.reset(new QuickSyncEncoder(filename, resource_pool, surface, va_display, width, height, oformat, stream_audio_encoder.get(), x264_encoder.get()));
	quicksync_encoder->set_stream_mux(stream_mux.get());
}

void VideoEncoder::add_audio(int64_t pts, std::vector<float> audio)
{
	quicksync_encoder->add_audio(pts, audio);
}

bool VideoEncoder::begin_frame(GLuint *y_tex, GLuint *cbcr_tex)
{
	return quicksync_encoder->begin_frame(y_tex, cbcr_tex);
}

RefCountedGLsync VideoEncoder::end_frame(int64_t pts, int64_t duration, const std::vector<RefCountedFrame> &input_frames)
{
	return quicksync_encoder->end_frame(pts, duration, input_frames);
}

void VideoEncoder::open_output_stream()
{
	AVFormatContext *avctx = avformat_alloc_context();
	avctx->oformat = oformat;

	uint8_t *buf = (uint8_t *)av_malloc(MUX_BUFFER_SIZE);
	avctx->pb = avio_alloc_context(buf, MUX_BUFFER_SIZE, 1, this, nullptr, &VideoEncoder::write_packet_thunk, nullptr);

	Mux::Codec video_codec;
	if (global_flags.uncompressed_video_to_http) {
		video_codec = Mux::CODEC_NV12;
	} else {
		video_codec = Mux::CODEC_H264;
	}

	avctx->flags = AVFMT_FLAG_CUSTOM_IO;

	string video_extradata;
	if (global_flags.x264_video_to_http) {
		video_extradata = x264_encoder->get_global_headers();
	}

	int time_base = global_flags.stream_coarse_timebase ? COARSE_TIMEBASE : TIMEBASE;
	stream_mux_writing_header = true;
	stream_mux.reset(new Mux(avctx, width, height, video_codec, video_extradata, stream_audio_encoder->get_ctx(), time_base, this));
	stream_mux_writing_header = false;
	httpd->set_header(stream_mux_header);
	stream_mux_header.clear();
}

void VideoEncoder::close_output_stream()
{
	stream_mux.reset();
}

int VideoEncoder::write_packet_thunk(void *opaque, uint8_t *buf, int buf_size)
{
	VideoEncoder *video_encoder = (VideoEncoder *)opaque;
	return video_encoder->write_packet(buf, buf_size);
}

int VideoEncoder::write_packet(uint8_t *buf, int buf_size)
{
	if (stream_mux_writing_header) {
		stream_mux_header.append((char *)buf, buf_size);
	} else {
		httpd->add_data((char *)buf, buf_size, stream_mux_writing_keyframes);
		stream_mux_writing_keyframes = false;
	}
	return buf_size;
}

