// A class to orchestrate the concept of video encoding. Will keep track of
// the muxes to stream and disk, the QuickSyncEncoder, and also the X264Encoder
// (for the stream) if there is one.

#ifndef _VIDEO_ENCODER_H
#define _VIDEO_ENCODER_H

#include <stdint.h>
#include <memory>
#include <string>
#include <vector>

#include "ref_counted_frame.h"
#include "ref_counted_gl_sync.h"

class HTTPD;
class QSurface;
class QuickSyncEncoder;

class VideoEncoder {
public:
	VideoEncoder(QSurface *surface, const std::string &va_display, int width, int height, HTTPD *httpd);
	~VideoEncoder();

	void add_audio(int64_t pts, std::vector<float> audio);
	bool begin_frame(GLuint *y_tex, GLuint *cbcr_tex);
	RefCountedGLsync end_frame(int64_t pts, int64_t duration, const std::vector<RefCountedFrame> &input_frames);

	// Does a cut of the disk stream immediately ("frame" is used for the filename only).
	void do_cut(int frame);

private:
	std::unique_ptr<QuickSyncEncoder> quicksync_encoder;
	QSurface *surface;
	std::string va_display;
	int width, height;
	HTTPD *httpd;
};

#endif
