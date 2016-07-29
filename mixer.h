#ifndef _MIXER_H
#define _MIXER_H 1

// The actual video mixer, running in its own separate background thread.

#include <epoxy/gl.h>
#undef Success
#include <stdbool.h>
#include <stdint.h>

#include <movit/effect_chain.h>
#include <movit/flat_input.h>
#include <zita-resampler/resampler.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "bmusb/bmusb.h"
#include "alsa_output.h"
#include "audio_mixer.h"
#include "ebu_r128_proc.h"
#include "video_encoder.h"
#include "httpd.h"
#include "pbo_frame_allocator.h"
#include "ref_counted_frame.h"
#include "ref_counted_gl_sync.h"
#include "resampling_queue.h"
#include "theme.h"
#include "timebase.h"
#include "stereocompressor.h"
#include "filter.h"
#include "input_state.h"
#include "correlation_measurer.h"

class QuickSyncEncoder;
class QSurface;
namespace movit {
class Effect;
class EffectChain;
class FlatInput;
class ResourcePool;
}  // namespace movit

namespace movit {
class YCbCrInput;
}
class QSurfaceFormat;

// For any card that's not the master (where we pick out the frames as they
// come, as fast as we can process), there's going to be a queue. The question
// is when we should drop frames from that queue (apart from the obvious
// dropping if the 16-frame queue should become full), especially given that
// the frame rate could be lower or higher than the master (either subtly or
// dramatically). We have two (conflicting) demands:
//
//   1. We want to avoid starving the queue.
//   2. We don't want to add more delay than is needed.
//
// Our general strategy is to drop as many frames as we can (helping for #2)
// that we think is safe for #1 given jitter. To this end, we set a lower floor N,
// where we assume that if we have N frames in the queue, we're always safe from
// starvation. (Typically, N will be 0 or 1. It starts off at 0.) If we have
// more than N frames in the queue after reading out the one we need, we head-drop
// them to reduce the queue.
//
// N is reduced as follows: If the queue has had at least one spare frame for
// at least 50 (master) frames (ie., it's been too conservative for a second),
// we reduce N by 1 and reset the timers. TODO: Only do this if N ever actually
// touched the limit.
//
// Whenever the queue is starved (we needed a frame but there was none),
// and we've been at N since the last starvation, N was obviously too low,
// so we increment it. We will never set N above 5, though.
class QueueLengthPolicy {
public:
	QueueLengthPolicy() {}
	void reset(unsigned card_index) {
		this->card_index = card_index;
		safe_queue_length = 0;
		frames_with_at_least_one = 0;
		been_at_safe_point_since_last_starvation = false;
	}

	void update_policy(int queue_length);  // Give in -1 for starvation.
	unsigned get_safe_queue_length() const { return safe_queue_length; }

private:
	unsigned card_index;  // For debugging only.
	unsigned safe_queue_length = 0;  // Called N in the comments.
	unsigned frames_with_at_least_one = 0;
	bool been_at_safe_point_since_last_starvation = false;
};

class Mixer {
public:
	// The surface format is used for offscreen destinations for OpenGL contexts we need.
	Mixer(const QSurfaceFormat &format, unsigned num_cards);
	~Mixer();
	void start();
	void quit();

	void transition_clicked(int transition_num);
	void channel_clicked(int preview_num);

	enum Output {
		OUTPUT_LIVE = 0,
		OUTPUT_PREVIEW,
		OUTPUT_INPUT0,  // 1, 2, 3, up to 15 follow numerically.
		NUM_OUTPUTS = 18
	};

	struct DisplayFrame {
		// The chain for rendering this frame. To render a display frame,
		// first wait for <ready_fence>, then call <setup_chain>
		// to wire up all the inputs, and then finally call
		// chain->render_to_screen() or similar.
		movit::EffectChain *chain;
		std::function<void()> setup_chain;

		// Asserted when all the inputs are ready; you cannot render the chain
		// before this.
		RefCountedGLsync ready_fence;

		// Holds on to all the input frames needed for this display frame,
		// so they are not released while still rendering.
		std::vector<RefCountedFrame> input_frames;

		// Textures that should be released back to the resource pool
		// when this frame disappears, if any.
		// TODO: Refcount these as well?
		std::vector<GLuint> temp_textures;
	};
	// Implicitly frees the previous one if there's a new frame available.
	bool get_display_frame(Output output, DisplayFrame *frame) {
		return output_channel[output].get_display_frame(frame);
	}

	typedef std::function<void()> new_frame_ready_callback_t;
	void set_frame_ready_callback(Output output, new_frame_ready_callback_t callback)
	{
		output_channel[output].set_frame_ready_callback(callback);
	}

	// TODO: Should this really be per-channel? Shouldn't it just be called for e.g. the live output?
	typedef std::function<void(const std::vector<std::string> &)> transition_names_updated_callback_t;
	void set_transition_names_updated_callback(Output output, transition_names_updated_callback_t callback)
	{
		output_channel[output].set_transition_names_updated_callback(callback);
	}

	typedef std::function<void(const std::string &)> name_updated_callback_t;
	void set_name_updated_callback(Output output, name_updated_callback_t callback)
	{
		output_channel[output].set_name_updated_callback(callback);
	}

	typedef std::function<void(const std::string &)> color_updated_callback_t;
	void set_color_updated_callback(Output output, color_updated_callback_t callback)
	{
		output_channel[output].set_color_updated_callback(callback);
	}

	typedef std::function<void(float level_lufs, float peak_db,
	                           float global_level_lufs, float range_low_lufs, float range_high_lufs,
	                           float gain_staging_db, float final_makeup_gain_db,
	                           float correlation)> audio_level_callback_t;
	void set_audio_level_callback(audio_level_callback_t callback)
	{
		audio_level_callback = callback;
	}

	std::vector<std::string> get_transition_names()
	{
		return theme->get_transition_names(pts());
	}

	unsigned get_num_channels() const
	{
		return theme->get_num_channels();
	}

	std::string get_channel_name(unsigned channel) const
	{
		return theme->get_channel_name(channel);
	}

	std::string get_channel_color(unsigned channel) const
	{
		return theme->get_channel_color(channel);
	}

	int get_channel_signal(unsigned channel) const
	{
		return theme->get_channel_signal(channel);
	}

	int map_signal(unsigned channel)
	{
		return theme->map_signal(channel);
	}

	unsigned get_audio_source() const
	{
		return audio_source_channel;
	}

	void set_audio_source(unsigned channel)
	{
		audio_source_channel = channel;
	}

	unsigned get_master_clock() const
	{
		return master_clock_channel;
	}

	void set_master_clock(unsigned channel)
	{
		master_clock_channel = channel;
	}

	void set_signal_mapping(int signal, int card)
	{
		return theme->set_signal_mapping(signal, card);
	}

	bool get_supports_set_wb(unsigned channel) const
	{
		return theme->get_supports_set_wb(channel);
	}

	void set_wb(unsigned channel, double r, double g, double b) const
	{
		theme->set_wb(channel, r, g, b);
	}

	AudioMixer *get_audio_mixer() { return &audio_mixer; }
	const AudioMixer *get_audio_mixer() const { return &audio_mixer; }

	void schedule_cut()
	{
		should_cut = true;
	}

	void reset_meters();

	unsigned get_num_cards() const { return num_cards; }

	std::string get_card_description(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_description();
	}

	std::map<uint32_t, bmusb::VideoMode> get_available_video_modes(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_available_video_modes();
	}

	uint32_t get_current_video_mode(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_current_video_mode();
	}

	void set_video_mode(unsigned card_index, uint32_t mode) {
		assert(card_index < num_cards);
		cards[card_index].capture->set_video_mode(mode);
	}

	void start_mode_scanning(unsigned card_index);

	std::map<uint32_t, std::string> get_available_video_inputs(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_available_video_inputs();
	}

	uint32_t get_current_video_input(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_current_video_input();
	}

	void set_video_input(unsigned card_index, uint32_t input) {
		assert(card_index < num_cards);
		cards[card_index].capture->set_video_input(input);
	}

	std::map<uint32_t, std::string> get_available_audio_inputs(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_available_audio_inputs();
	}

	uint32_t get_current_audio_input(unsigned card_index) const {
		assert(card_index < num_cards);
		return cards[card_index].capture->get_current_audio_input();
	}

	void set_audio_input(unsigned card_index, uint32_t input) {
		assert(card_index < num_cards);
		cards[card_index].capture->set_audio_input(input);
	}

	void change_x264_bitrate(unsigned rate_kbit) {
		video_encoder->change_x264_bitrate(rate_kbit);
	}

private:
	void configure_card(unsigned card_index, bmusb::CaptureInterface *capture, bool is_fake_capture);
	void bm_frame(unsigned card_index, uint16_t timecode,
		bmusb::FrameAllocator::Frame video_frame, size_t video_offset, bmusb::VideoFormat video_format,
		bmusb::FrameAllocator::Frame audio_frame, size_t audio_offset, bmusb::AudioFormat audio_format);
	void bm_hotplug_add(libusb_device *dev);
	void bm_hotplug_remove(unsigned card_index);
	void place_rectangle(movit::Effect *resample_effect, movit::Effect *padding_effect, float x0, float y0, float x1, float y1);
	void thread_func();
	void handle_hotplugged_cards();
	void schedule_audio_resampling_tasks(unsigned dropped_frames, int num_samples_per_frame, int length_per_frame);
	void render_one_frame(int64_t duration);
	void send_audio_level_callback();
	void audio_thread_func();
	void process_audio_one_frame(int64_t frame_pts_int, int num_samples, ResamplingQueue::RateAdjustmentPolicy rate_adjustment_policy);
	void subsample_chroma(GLuint src_tex, GLuint dst_dst);
	void release_display_frame(DisplayFrame *frame);
	double pts() { return double(pts_int) / TIMEBASE; }

	HTTPD httpd;
	unsigned num_cards;

	QSurface *mixer_surface, *h264_encoder_surface;
	std::unique_ptr<movit::ResourcePool> resource_pool;
	std::unique_ptr<Theme> theme;
	std::atomic<unsigned> audio_source_channel{0};
	std::atomic<unsigned> master_clock_channel{0};
	std::unique_ptr<movit::EffectChain> display_chain;
	GLuint cbcr_program_num;  // Owned by <resource_pool>.
	GLuint cbcr_vbo;  // Holds position and texcoord data.
	GLuint cbcr_position_attribute_index, cbcr_texcoord_attribute_index;
	std::unique_ptr<VideoEncoder> video_encoder;

	// Effects part of <display_chain>. Owned by <display_chain>.
	movit::FlatInput *display_input;

	int64_t pts_int = 0;  // In TIMEBASE units.

	std::mutex bmusb_mutex;
	bool has_bmusb_thread = false;
	struct CaptureCard {
		bmusb::CaptureInterface *capture = nullptr;
		bool is_fake_capture;
		std::unique_ptr<PBOFrameAllocator> frame_allocator;

		// Stuff for the OpenGL context (for texture uploading).
		QSurface *surface = nullptr;

		struct NewFrame {
			RefCountedFrame frame;
			int64_t length;  // In TIMEBASE units.
			bool interlaced;
			unsigned field;  // Which field (0 or 1) of the frame to use. Always 0 for progressive.
			std::function<void()> upload_func;  // Needs to be called to actually upload the texture to OpenGL.
			unsigned dropped_frames = 0;  // Number of dropped frames before this one.
		};
		std::queue<NewFrame> new_frames;
		bool should_quit = false;
		std::condition_variable new_frames_changed;  // Set whenever new_frames (or should_quit) is changed.

		QueueLengthPolicy queue_length_policy;  // Refers to the "new_frames" queue.

		// Accumulated errors in number of 1/TIMEBASE samples. If OUTPUT_FREQUENCY divided by
		// frame rate is integer, will always stay zero.
		unsigned fractional_samples = 0;

		int last_timecode = -1;  // Unwrapped.
	};
	CaptureCard cards[MAX_CARDS];  // protected by <bmusb_mutex>
	AudioMixer audio_mixer;
	void get_one_frame_from_each_card(unsigned master_card_index, CaptureCard::NewFrame new_frames[MAX_CARDS], bool has_new_frame[MAX_CARDS], int num_samples[MAX_CARDS]);

	InputState input_state;

	// Cards we have been noticed about being hotplugged, but haven't tried adding yet.
	// Protected by its own mutex.
	std::mutex hotplug_mutex;
	std::vector<libusb_device *> hotplugged_cards;

	class OutputChannel {
	public:
		~OutputChannel();
		void output_frame(DisplayFrame frame);
		bool get_display_frame(DisplayFrame *frame);
		void set_frame_ready_callback(new_frame_ready_callback_t callback);
		void set_transition_names_updated_callback(transition_names_updated_callback_t callback);
		void set_name_updated_callback(name_updated_callback_t callback);
		void set_color_updated_callback(color_updated_callback_t callback);

	private:
		friend class Mixer;

		unsigned channel;
		Mixer *parent = nullptr;  // Not owned.
		std::mutex frame_mutex;
		DisplayFrame current_frame, ready_frame;  // protected by <frame_mutex>
		bool has_current_frame = false, has_ready_frame = false;  // protected by <frame_mutex>
		new_frame_ready_callback_t new_frame_ready_callback;
		transition_names_updated_callback_t transition_names_updated_callback;
		name_updated_callback_t name_updated_callback;
		color_updated_callback_t color_updated_callback;

		std::vector<std::string> last_transition_names;
		std::string last_name, last_color;
	};
	OutputChannel output_channel[NUM_OUTPUTS];

	std::thread mixer_thread;
	std::thread audio_thread;
	std::atomic<bool> should_quit{false};
	std::atomic<bool> should_cut{false};

	audio_level_callback_t audio_level_callback = nullptr;
	mutable std::mutex audio_measure_mutex;
	Ebu_r128_proc r128;  // Under audio_measure_mutex.
	CorrelationMeasurer correlation;  // Under audio_measure_mutex.
	Resampler peak_resampler;  // Under audio_measure_mutex.
	std::atomic<float> peak{0.0f};

	std::unique_ptr<ALSAOutput> alsa;

	struct AudioTask {
		int64_t pts_int;
		int num_samples;
		bool adjust_rate;
	};
	std::mutex audio_mutex;
	std::condition_variable audio_task_queue_changed;
	std::queue<AudioTask> audio_task_queue;  // Under audio_mutex.

	// For mode scanning.
	bool is_mode_scanning[MAX_CARDS]{ false };
	std::vector<uint32_t> mode_scanlist[MAX_CARDS];
	unsigned mode_scanlist_index[MAX_CARDS]{ 0 };
	std::chrono::steady_clock::time_point last_mode_scan_change[MAX_CARDS];
};

extern Mixer *global_mixer;
extern bool uses_mlock;

#endif  // !defined(_MIXER_H)
