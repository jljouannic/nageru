#ifndef _ALSA_INPUT_H
#define _ALSA_INPUT_H 1

// ALSA sound input, running in a separate thread and sending audio back
// in callbacks.
//
// Note: “frame” here generally refers to the ALSA definition of frame,
// which is a set of samples, exactly one for each channel. The only exception
// is in frame_length, where it means the TIMEBASE length of the buffer
// as a whole, since that's what AudioMixer::add_audio() wants.

#include <alsa/asoundlib.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "bmusb/bmusb.h"
#include "timebase.h"

class ALSAInput {
public:
	typedef std::function<void(const uint8_t *data, unsigned num_samples, bmusb::AudioFormat audio_format, int64_t frame_length)> audio_callback_t;

	ALSAInput(const char *device, unsigned sample_rate, unsigned num_channels, audio_callback_t audio_callback);
	~ALSAInput();

	// NOTE: Might very well be different from the sample rate given to the
	// constructor, since the card might not support the one you wanted.
	unsigned get_sample_rate() const { return sample_rate; }

	void start_capture_thread();
	void stop_capture_thread();

	// TODO: Worry about hotplug.
	struct Device {
		std::string address;  // E.g. “hw:0,0”.
		std::string name, info;
		unsigned num_channels;
	};
	static std::vector<Device> enumerate_devices();

private:
	void capture_thread_func();
	int64_t frames_to_pts(uint64_t n) const;
	void die_on_error(const char *func_name, int err);

	std::string device;
	unsigned sample_rate, num_channels, num_periods;
	snd_pcm_uframes_t period_size;
	snd_pcm_uframes_t buffer_frames;
	bmusb::AudioFormat audio_format;
	audio_callback_t audio_callback;

	snd_pcm_t *pcm_handle;
	std::thread capture_thread;
	std::atomic<bool> should_quit{false};
	std::unique_ptr<uint8_t[]> buffer;
};

#endif  // !defined(_ALSA_INPUT_H)
