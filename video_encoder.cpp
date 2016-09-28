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

VideoEncoder::VideoEncoder(ResourcePool *resource_pool, QSurface *surface, const std::string &va_display, int width, int height, HTTPD *httpd, DiskSpaceEstimator *disk_space_estimator)
	: resource_pool(resource_pool), surface(surface), va_display(va_display), width(width), height(height), httpd(httpd), disk_space_estimator(disk_space_estimator)
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
	quicksync_encoder.reset(new QuickSyncEncoder(filename, resource_pool, surface, va_display, width, height, oformat, x264_encoder.get(), disk_space_estimator));

	open_output_stream();
	stream_audio_encoder->add_mux(stream_mux.get());
	quicksync_encoder->set_stream_mux(stream_mux.get());
	if (global_flags.x264_video_to_http) {
		x264_encoder->set_mux(stream_mux.get());
	}
}

VideoEncoder::~VideoEncoder()
{
	quicksync_encoder.reset(nullptr);
	while (quicksync_encoders_in_shutdown.load() > 0) {
		usleep(10000);
	}
}

void VideoEncoder::do_cut(int frame)
{
	string filename = generate_local_dump_filename(frame);
	printf("Starting new recording: %s\n", filename.c_str());

	// Do the shutdown of the old encoder in a separate thread, since it can
	// take some time (it needs to wait for all the frames in the queue to be
	// done encoding, for one) and we are running on the main mixer thread.
	// However, since this means both encoders could be sending packets at
	// the same time, it means pts could come out of order to the stream mux,
	// and we need to plug it until the shutdown is complete.
	stream_mux->plug();
	lock_guard<mutex> lock(qs_mu);
	QuickSyncEncoder *old_encoder = quicksync_encoder.release();  // When we go C++14, we can use move capture instead.
	thread([old_encoder, this]{
		old_encoder->shutdown();
		stream_mux->unplug();

		// We cannot delete the encoder here, as this thread has no OpenGL context.
		// We'll deal with it in begin_frame().
		lock_guard<mutex> lock(qs_mu);
		qs_needing_cleanup.emplace_back(old_encoder);
	}).detach();

	quicksync_encoder.reset(new QuickSyncEncoder(filename, resource_pool, surface, va_display, width, height, oformat, x264_encoder.get(), disk_space_estimator));
	quicksync_encoder->set_stream_mux(stream_mux.get());
}

void VideoEncoder::change_x264_bitrate(unsigned rate_kbit)
{
	x264_encoder->change_bitrate(rate_kbit);
}

void VideoEncoder::add_audio(int64_t pts, std::vector<float> audio)
{
	lock_guard<mutex> lock(qs_mu);
	quicksync_encoder->add_audio(pts, audio);
	stream_audio_encoder->encode_audio(audio, pts + quicksync_encoder->global_delay());
}

bool VideoEncoder::begin_frame(GLuint *y_tex, GLuint *cbcr_tex)
{
	lock_guard<mutex> lock(qs_mu);
	qs_needing_cleanup.clear();  // Since we have an OpenGL context here, and are called regularly.
	return quicksync_encoder->begin_frame(y_tex, cbcr_tex);
}

RefCountedGLsync VideoEncoder::end_frame(int64_t pts, int64_t duration, const std::vector<RefCountedFrame> &input_frames)
{
	lock_guard<mutex> lock(qs_mu);
	return quicksync_encoder->end_frame(pts, duration, input_frames);
}

void VideoEncoder::open_output_stream()
{
	AVFormatContext *avctx = avformat_alloc_context();
	avctx->oformat = oformat;

	uint8_t *buf = (uint8_t *)av_malloc(MUX_BUFFER_SIZE);
	avctx->pb = avio_alloc_context(buf, MUX_BUFFER_SIZE, 1, this, nullptr, nullptr, nullptr);
	avctx->pb->write_data_type = &VideoEncoder::write_packet2_thunk;
	avctx->pb->ignore_boundary_point = 1;

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
	stream_mux.reset(new Mux(avctx, width, height, video_codec, video_extradata, stream_audio_encoder->get_codec_parameters().get(), time_base,
		/*write_callback=*/nullptr));
}

int VideoEncoder::write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
	VideoEncoder *video_encoder = (VideoEncoder *)opaque;
	return video_encoder->write_packet2(buf, buf_size, type, time);
}

int VideoEncoder::write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time)
{
	if (type == AVIO_DATA_MARKER_HEADER) {
		stream_mux_header.append((char *)buf, buf_size);
		httpd->set_header(stream_mux_header);
	} else {
		httpd->add_data((char *)buf, buf_size, type == AVIO_DATA_MARKER_SYNC_POINT);
	}
	return buf_size;
}

