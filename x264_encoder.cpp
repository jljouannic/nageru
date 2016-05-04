#include <string.h>
#include <unistd.h>

#include "defs.h"
#include "flags.h"
#include "mux.h"
#include "timebase.h"
#include "x264_encoder.h"
#include "x264_speed_control.h"

extern "C" {
#include <libavformat/avformat.h>
}

using namespace std;

X264Encoder::X264Encoder(AVOutputFormat *oformat)
	: wants_global_headers(oformat->flags & AVFMT_GLOBALHEADER)
{
	frame_pool.reset(new uint8_t[WIDTH * HEIGHT * 2 * X264_QUEUE_LENGTH]);
	for (unsigned i = 0; i < X264_QUEUE_LENGTH; ++i) {
		free_frames.push(frame_pool.get() + i * (WIDTH * HEIGHT * 2));
	}
	encoder_thread = thread(&X264Encoder::encoder_thread_func, this);
}

X264Encoder::~X264Encoder()
{
	should_quit = true;
	queued_frames_nonempty.notify_all();
	encoder_thread.join();
}

void X264Encoder::add_frame(int64_t pts, int64_t duration, const uint8_t *data)
{
	QueuedFrame qf;
	qf.pts = pts;
	qf.duration = duration;

	{
		lock_guard<mutex> lock(mu);
		if (free_frames.empty()) {
			fprintf(stderr, "WARNING: x264 queue full, dropping frame with pts %ld\n", pts);
			return;
		}

		qf.data = free_frames.front();
		free_frames.pop();
	}

	memcpy(qf.data, data, WIDTH * HEIGHT * 2);

	{
		lock_guard<mutex> lock(mu);
		queued_frames.push(qf);
		queued_frames_nonempty.notify_all();
	}
}
	
void X264Encoder::init_x264()
{
	x264_param_t param;
	x264_param_default_preset(&param, global_flags.x264_preset.c_str(), global_flags.x264_tune.c_str());

	param.i_width = WIDTH;
	param.i_height = HEIGHT;
	param.i_csp = X264_CSP_NV12;
	param.b_vfr_input = 1;
	param.i_timebase_num = 1;
	param.i_timebase_den = TIMEBASE;
	param.i_keyint_max = 50; // About one second.
	if (global_flags.x264_speedcontrol) {
		param.i_frame_reference = 16;  // Because speedcontrol is never allowed to change this above what we set at start.
	}

	// NOTE: These should be in sync with the ones in h264encode.cpp (sbs_rbsp()).
	param.vui.i_vidformat = 5;  // Unspecified.
	param.vui.b_fullrange = 0;
	param.vui.i_colorprim = 1;  // BT.709.
	param.vui.i_transfer = 2;  // Unspecified (since we use sRGB).
	param.vui.i_colmatrix = 6;  // BT.601/SMPTE 170M.


	param.rc.i_rc_method = X264_RC_ABR;
	param.rc.i_bitrate = global_flags.x264_bitrate;
	if (global_flags.x264_vbv_buffer_size < 0) {
		param.rc.i_vbv_buffer_size = param.rc.i_bitrate;  // One-second VBV.
	} else {
		param.rc.i_vbv_buffer_size = global_flags.x264_vbv_buffer_size;
	}
	if (global_flags.x264_vbv_max_bitrate < 0) {
		param.rc.i_vbv_max_bitrate = param.rc.i_bitrate;  // CBR.
	} else {
		param.rc.i_vbv_max_bitrate = global_flags.x264_vbv_max_bitrate;
	}

	// Occasionally players have problem with extremely low quantizers;
	// be on the safe side. Shouldn't affect quality in any meaningful way.
	param.rc.i_qp_min = 5;

	// TODO: more flags here, via x264_param_parse().

	x264_param_apply_profile(&param, "high");

	param.b_repeat_headers = !wants_global_headers;

	x264 = x264_encoder_open(&param);
	if (x264 == nullptr) {
		fprintf(stderr, "ERROR: x264 initialization failed.\n");
		exit(1);
	}

	if (global_flags.x264_speedcontrol) {
		speed_control.reset(new X264SpeedControl(x264, /*f_speed=*/1.0f, X264_QUEUE_LENGTH, /*f_buffer_init=*/1.0f));
	}

	if (wants_global_headers) {
		x264_nal_t *nal;
		int num_nal;

		x264_encoder_headers(x264, &nal, &num_nal);

		for (int i = 0; i < num_nal; ++i) {
			if (nal[i].i_type == NAL_SEI) {
				// Don't put the SEI in extradata; make it part of the first frame instead.
				buffered_sei += string((const char *)nal[i].p_payload, nal[i].i_payload);
			} else {
				global_headers += string((const char *)nal[i].p_payload, nal[i].i_payload);
			}
		}
	}
}

void X264Encoder::encoder_thread_func()
{
	nice(5);  // Note that x264 further nices some of its threads.
	init_x264();

	bool frames_left;

	do {
		QueuedFrame qf;

		// Wait for a queued frame, then dequeue it.
		{
			unique_lock<mutex> lock(mu);
			queued_frames_nonempty.wait(lock, [this]() { return !queued_frames.empty() || should_quit; });
			if (!queued_frames.empty()) {
				qf = queued_frames.front();
				queued_frames.pop();
			} else {
				qf.pts = -1;
				qf.duration = -1;
				qf.data = nullptr;
			}

			frames_left = !queued_frames.empty();
		}

		encode_frame(qf);
		
		{
			lock_guard<mutex> lock(mu);
			free_frames.push(qf.data);
		}

		// We should quit only if the should_quit flag is set _and_ we have nothing
		// in either queue.
	} while (!should_quit || frames_left || x264_encoder_delayed_frames(x264) > 0);

	x264_encoder_close(x264);
}

void X264Encoder::encode_frame(X264Encoder::QueuedFrame qf)
{
	x264_nal_t *nal = nullptr;
	int num_nal = 0;
	x264_picture_t pic;
	x264_picture_t *input_pic = nullptr;

	if (qf.data) {
		x264_picture_init(&pic);

		pic.i_pts = qf.pts;
		pic.img.i_csp = X264_CSP_NV12;
		pic.img.i_plane = 2;
		pic.img.plane[0] = qf.data;
		pic.img.i_stride[0] = WIDTH;
		pic.img.plane[1] = qf.data + WIDTH * HEIGHT;
		pic.img.i_stride[1] = WIDTH / 2 * sizeof(uint16_t);
		pic.opaque = reinterpret_cast<void *>(intptr_t(qf.duration));

		input_pic = &pic;
	}

	if (speed_control) {
		speed_control->before_frame(float(free_frames.size()) / X264_QUEUE_LENGTH, X264_QUEUE_LENGTH, 1e6 * qf.duration / TIMEBASE);
	}
	x264_encoder_encode(x264, &nal, &num_nal, input_pic, &pic);
	if (speed_control) {
		speed_control->after_frame();
	}

	// We really need one AVPacket for the entire frame, it seems,
	// so combine it all.
	size_t num_bytes = buffered_sei.size();
	for (int i = 0; i < num_nal; ++i) {
		num_bytes += nal[i].i_payload;
	}

	unique_ptr<uint8_t[]> data(new uint8_t[num_bytes]);
	uint8_t *ptr = data.get();

	if (!buffered_sei.empty()) {
		memcpy(ptr, buffered_sei.data(), buffered_sei.size());
		ptr += buffered_sei.size();
		buffered_sei.clear();
	}
	for (int i = 0; i < num_nal; ++i) {
		memcpy(ptr, nal[i].p_payload, nal[i].i_payload);
		ptr += nal[i].i_payload;
	}

	AVPacket pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.buf = nullptr;
	pkt.data = data.get();
	pkt.size = num_bytes;
	pkt.stream_index = 0;
	if (pic.b_keyframe) {
		pkt.flags = AV_PKT_FLAG_KEY;
	} else {
		pkt.flags = 0;
	}
	pkt.duration = reinterpret_cast<intptr_t>(pic.opaque);

	mux->add_packet(pkt, pic.i_pts, pic.i_dts);
}
