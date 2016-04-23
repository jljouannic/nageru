#include "video_encoder.h"

#include <string>

#include "defs.h"
#include "quicksync_encoder.h"

using namespace std;

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

VideoEncoder::VideoEncoder(QSurface *surface, const std::string &va_display, int width, int height, HTTPD *httpd)
	: surface(surface), va_display(va_display), width(width), height(height), httpd(httpd)
{
	quicksync_encoder.reset(new QuickSyncEncoder(surface, va_display, width, height, httpd));
	quicksync_encoder->open_output_file(generate_local_dump_filename(/*frame=*/0).c_str());
}

VideoEncoder::~VideoEncoder()
{
	quicksync_encoder.reset(nullptr);
}

void VideoEncoder::do_cut(int frame)
{
	string filename = generate_local_dump_filename(frame);
	printf("Starting new recording: %s\n", filename.c_str());
	quicksync_encoder->close_output_file();
	quicksync_encoder->shutdown();
	quicksync_encoder.reset(new QuickSyncEncoder(surface, va_display, width, height, httpd));
	quicksync_encoder->open_output_file(filename.c_str());
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
