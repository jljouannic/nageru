// A class to orchestrate the concept of video encoding. Will keep track of
// the muxes to stream and disk, the QuickSyncEncoder, and also the X264Encoder
// (for the stream) if there is one.

#ifndef _VIDEO_ENCODER_H
#define _VIDEO_ENCODER_H

#include <stdint.h>
#include <memory>
#include <string>
#include <vector>

#include "audio_encoder.h"
#include "mux.h"
#include "ref_counted_frame.h"
#include "ref_counted_gl_sync.h"

class HTTPD;
class QSurface;
class QuickSyncEncoder;
class X264Encoder;

namespace movit {
class ResourcePool;
}  // namespace movit

class VideoEncoder : public KeyFrameSignalReceiver {
public:
	VideoEncoder(movit::ResourcePool *resource_pool, QSurface *surface, const std::string &va_display, int width, int height, HTTPD *httpd);
	~VideoEncoder();

	void add_audio(int64_t pts, std::vector<float> audio);
	bool begin_frame(GLuint *y_tex, GLuint *cbcr_tex);
	RefCountedGLsync end_frame(int64_t pts, int64_t duration, const std::vector<RefCountedFrame> &input_frames);

	// Does a cut of the disk stream immediately ("frame" is used for the filename only).
	void do_cut(int frame);

	virtual void signal_keyframe() override {
		stream_mux_writing_keyframes = true;
	}

private:
	void open_output_stream();
	void close_output_stream();
	static int write_packet_thunk(void *opaque, uint8_t *buf, int buf_size);
	int write_packet(uint8_t *buf, int buf_size);

	AVOutputFormat *oformat;
	std::unique_ptr<QuickSyncEncoder> quicksync_encoder;
	movit::ResourcePool *resource_pool;
	QSurface *surface;
	std::string va_display;
	int width, height;
	HTTPD *httpd;

	std::unique_ptr<Mux> stream_mux;  // To HTTP.
	std::unique_ptr<AudioEncoder> stream_audio_encoder;
	std::unique_ptr<X264Encoder> x264_encoder;  // nullptr if not using x264.

	// While Mux object is constructing, <stream_mux_writing_header> is true,
	// and the header is being collected into stream_mux_header.
	bool stream_mux_writing_header;
	std::string stream_mux_header;

	bool stream_mux_writing_keyframes = false;
};

#endif
