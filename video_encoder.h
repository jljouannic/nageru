// A class to orchestrate the concept of video encoding. Will keep track of
// the muxes to stream and disk, the QuickSyncEncoder, and also the X264Encoder
// (for the stream) if there is one.

#ifndef _VIDEO_ENCODER_H
#define _VIDEO_ENCODER_H

#include <stdint.h>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "audio_encoder.h"
#include "mux.h"
#include "ref_counted_frame.h"
#include "ref_counted_gl_sync.h"

class DiskSpaceEstimator;
class HTTPD;
class QSurface;
class QuickSyncEncoder;
class X264Encoder;

namespace movit {
class ResourcePool;
}  // namespace movit

class VideoEncoder {
public:
	VideoEncoder(movit::ResourcePool *resource_pool, QSurface *surface, const std::string &va_display, int width, int height, HTTPD *httpd, DiskSpaceEstimator *disk_space_estimator);
	~VideoEncoder();

	void add_audio(int64_t pts, std::vector<float> audio);
	bool begin_frame(GLuint *y_tex, GLuint *cbcr_tex);
	RefCountedGLsync end_frame(int64_t pts, int64_t duration, const std::vector<RefCountedFrame> &input_frames);

	// Does a cut of the disk stream immediately ("frame" is used for the filename only).
	void do_cut(int frame);

	void change_x264_bitrate(unsigned rate_kbit);

private:
	void open_output_stream();
	static int write_packet2_thunk(void *opaque, uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);
	int write_packet2(uint8_t *buf, int buf_size, AVIODataMarkerType type, int64_t time);

	AVOutputFormat *oformat;
	std::mutex qs_mu;
	std::unique_ptr<QuickSyncEncoder> quicksync_encoder;  // Under <qs_mu>.
	movit::ResourcePool *resource_pool;
	QSurface *surface;
	std::string va_display;
	int width, height;
	HTTPD *httpd;
	DiskSpaceEstimator *disk_space_estimator;

	std::unique_ptr<Mux> stream_mux;  // To HTTP.
	std::unique_ptr<AudioEncoder> stream_audio_encoder;
	std::unique_ptr<X264Encoder> x264_encoder;  // nullptr if not using x264.

	std::string stream_mux_header;

	std::atomic<int> quicksync_encoders_in_shutdown{0};

	// Encoders that are shutdown, but need to call release_gl_resources()
	// (or be deleted) from some thread with an OpenGL context.
	std::vector<std::unique_ptr<QuickSyncEncoder>> qs_needing_cleanup;  // Under <qs_mu>.
};

#endif
