#ifndef _QUICKSYNC_ENCODER_IMPL_H
#define _QUICKSYNC_ENCODER_IMPL_H 1

#include <epoxy/egl.h>
#include <va/va.h>

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <stack>
#include <thread>

#include "audio_encoder.h"
#include "defs.h"
#include "timebase.h"
#include "print_latency.h"

#define SURFACE_NUM 16 /* 16 surfaces for source YUV */
#define MAX_NUM_REF1 16 // Seemingly a hardware-fixed value, not related to SURFACE_NUM
#define MAX_NUM_REF2 32 // Seemingly a hardware-fixed value, not related to SURFACE_NUM

struct __bitstream {
    unsigned int *buffer;
    int bit_offset;
    int max_size_in_dword;
};
typedef struct __bitstream bitstream;

// H.264 video comes out in encoding order (e.g. with two B-frames:
// 0, 3, 1, 2, 6, 4, 5, etc.), but uncompressed video needs to
// come in the right order. Since we do everything, including waiting
// for the frames to come out of OpenGL, in encoding order, we need
// a reordering buffer for uncompressed frames so that they come out
// correctly. We go the super-lazy way of not making it understand
// anything about the true order (which introduces some extra latency,
// though); we know that for N B-frames we need at most (N-1) frames
// in the reorder buffer, and can just sort on that.
//
// The class also deals with keeping a freelist as needed.
class FrameReorderer {
public:
	FrameReorderer(unsigned queue_length, int width, int height);

	struct Frame {
		int64_t pts, duration;
		uint8_t *data;
		ReceivedTimestamps received_ts;

		// Invert to get the smallest pts first.
		bool operator< (const Frame &other) const { return pts > other.pts; }
	};

	// Returns the next frame to insert with its pts, if any. Otherwise -1 and nullptr.
	// Does _not_ take ownership of data; a copy is taken if needed.
	// The returned pointer is valid until the next call to reorder_frame, or destruction.
	// As a special case, if queue_length == 0, will just return pts and data (no reordering needed).
	Frame reorder_frame(int64_t pts, int64_t duration, uint8_t *data, const ReceivedTimestamps &received_ts);

	// The same as reorder_frame, but without inserting anything. Used to empty the queue.
	Frame get_first_frame();

	bool empty() const { return frames.empty(); }

private:
	unsigned queue_length;
	int width, height;

	std::priority_queue<Frame> frames;
	std::stack<uint8_t *> freelist;  // Includes the last value returned from reorder_frame.

	// Owns all the pointers. Normally, freelist and frames could do this themselves,
	// except priority_queue doesn't work well with movable-only types.
	std::vector<std::unique_ptr<uint8_t[]>> owner;
};


class QuickSyncEncoderImpl {
public:
	QuickSyncEncoderImpl(const std::string &filename, movit::ResourcePool *resource_pool, QSurface *surface, const std::string &va_display, int width, int height, AVOutputFormat *oformat, X264Encoder *x264_encoder, DiskSpaceEstimator *disk_space_estimator);
	~QuickSyncEncoderImpl();
	void add_audio(int64_t pts, std::vector<float> audio);
	bool begin_frame(GLuint *y_tex, GLuint *cbcr_tex);
	RefCountedGLsync end_frame(int64_t pts, int64_t duration, const std::vector<RefCountedFrame> &input_frames);
	void shutdown();
	void release_gl_resources();
	void set_stream_mux(Mux *mux)
	{
		stream_mux = mux;
	}

	// So we never get negative dts.
	int64_t global_delay() const {
		return int64_t(ip_period - 1) * (TIMEBASE / MAX_FPS);
	}

private:
	struct storage_task {
		unsigned long long display_order;
		int frame_type;
		std::vector<float> audio;
		int64_t pts, dts, duration;
		ReceivedTimestamps received_ts;
	};
	struct PendingFrame {
		RefCountedGLsync fence;
		std::vector<RefCountedFrame> input_frames;
		int64_t pts, duration;
	};

	void open_output_file(const std::string &filename);
	void encode_thread_func();
	void encode_remaining_frames_as_p(int encoding_frame_num, int gop_start_display_frame_num, int64_t last_dts);
	void add_packet_for_uncompressed_frame(int64_t pts, int64_t duration, const uint8_t *data);
	void encode_frame(PendingFrame frame, int encoding_frame_num, int display_frame_num, int gop_start_display_frame_num,
	                  int frame_type, int64_t pts, int64_t dts, int64_t duration);
	void storage_task_thread();
	void storage_task_enqueue(storage_task task);
	void save_codeddata(storage_task task);
	int render_packedsequence();
	int render_packedpicture();
	void render_packedslice();
	int render_sequence();
	int render_picture(int frame_type, int display_frame_num, int gop_start_display_frame_num);
	void sps_rbsp(bitstream *bs);
	void pps_rbsp(bitstream *bs);
	int build_packed_pic_buffer(unsigned char **header_buffer);
	int render_slice(int encoding_frame_num, int display_frame_num, int gop_start_display_frame_num, int frame_type);
	void slice_header(bitstream *bs);
	int build_packed_seq_buffer(unsigned char **header_buffer);
	int build_packed_slice_buffer(unsigned char **header_buffer);
	int init_va(const std::string &va_display);
	int deinit_va();
	void enable_zerocopy_if_possible();
	VADisplay va_open_display(const std::string &va_display);
	void va_close_display(VADisplay va_dpy);
	int setup_encode();
	void release_encode();
	void update_ReferenceFrames(int frame_type);
	int update_RefPicList(int frame_type);

	bool is_shutdown = false;
	bool has_released_gl_resources = false;
	bool use_zerocopy;
	int drm_fd = -1;

	std::thread encode_thread, storage_thread;

	std::mutex storage_task_queue_mutex;
	std::condition_variable storage_task_queue_changed;
	int srcsurface_status[SURFACE_NUM];  // protected by storage_task_queue_mutex
	std::queue<storage_task> storage_task_queue;  // protected by storage_task_queue_mutex
	bool storage_thread_should_quit = false;  // protected by storage_task_queue_mutex

	std::mutex frame_queue_mutex;
	std::condition_variable frame_queue_nonempty;
	bool encode_thread_should_quit = false;  // under frame_queue_mutex

	int current_storage_frame;

	std::map<int, PendingFrame> pending_video_frames;  // under frame_queue_mutex
	movit::ResourcePool *resource_pool;
	QSurface *surface;

	std::unique_ptr<AudioEncoder> file_audio_encoder;

	std::unique_ptr<FrameReorderer> reorderer;
	X264Encoder *x264_encoder;  // nullptr if not using x264.

	Mux* stream_mux = nullptr;  // To HTTP.
	std::unique_ptr<Mux> file_mux;  // To local disk.

	Display *x11_display = nullptr;

	// Encoder parameters
	VADisplay va_dpy;
	VAProfile h264_profile = (VAProfile)~0;
	VAConfigAttrib config_attrib[VAConfigAttribTypeMax];
	int config_attrib_num = 0, enc_packed_header_idx;

	struct GLSurface {
		VASurfaceID src_surface, ref_surface;
		VABufferID coded_buf;

		VAImage surface_image;
		GLuint y_tex, cbcr_tex;

		// Only if use_zerocopy == true.
		EGLImage y_egl_image, cbcr_egl_image;

		// Only if use_zerocopy == false.
		GLuint pbo;
		uint8_t *y_ptr, *cbcr_ptr;
		size_t y_offset, cbcr_offset;
	};
	GLSurface gl_surfaces[SURFACE_NUM];

	VAConfigID config_id;
	VAContextID context_id;
	VAEncSequenceParameterBufferH264 seq_param;
	VAEncPictureParameterBufferH264 pic_param;
	VAEncSliceParameterBufferH264 slice_param;
	VAPictureH264 CurrentCurrPic;
	VAPictureH264 ReferenceFrames[MAX_NUM_REF1], RefPicList0_P[MAX_NUM_REF2], RefPicList0_B[MAX_NUM_REF2], RefPicList1_B[MAX_NUM_REF2];

	// Static quality settings.
	static constexpr unsigned int frame_bitrate = 15000000 / 60;  // Doesn't really matter; only initial_qp does.
	static constexpr unsigned int num_ref_frames = 2;
	static constexpr int initial_qp = 15;
	static constexpr int minimal_qp = 0;
	static constexpr int intra_period = 30;
	static constexpr int intra_idr_period = MAX_FPS;  // About a second; more at lower frame rates. Not ideal.

	// Quality settings that are meant to be static, but might be overridden
	// by the profile.
	int constraint_set_flag = 0;
	int h264_packedheader = 0; /* support pack header? */
	int h264_maxref = (1<<16|1);
	int h264_entropy_mode = 1; /* cabac */
	int ip_period = 3;

	int rc_mode = -1;
	unsigned int current_frame_num = 0;
	unsigned int numShortTerm = 0;

	int frame_width;
	int frame_height;
	int frame_width_mbaligned;
	int frame_height_mbaligned;

	DiskSpaceEstimator *disk_space_estimator;
};

#endif  // !defined(_QUICKSYNC_ENCODER_IMPL_H)
