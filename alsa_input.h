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
#include <unordered_map>
#include <vector>

#include "bmusb/bmusb.h"
#include "timebase.h"

class ALSAPool;

class ALSAInput {
public:
	typedef std::function<bool(const uint8_t *data, unsigned num_samples, bmusb::AudioFormat audio_format, int64_t frame_length)> audio_callback_t;

	ALSAInput(const char *device, unsigned sample_rate, unsigned num_channels, audio_callback_t audio_callback, ALSAPool *parent_pool, unsigned internal_dev_index);
	~ALSAInput();

	// NOTE: Might very well be different from the sample rate given to the
	// constructor, since the card might not support the one you wanted.
	unsigned get_sample_rate() const { return sample_rate; }

	void start_capture_thread();
	void stop_capture_thread();

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
	ALSAPool *parent_pool;
	unsigned internal_dev_index;
};

// The class dealing with the collective of all ALSA cards in the system.
// In particular, it deals with enumeration of cards, and hotplug of new ones.
class ALSAPool {
public:
	~ALSAPool();

	struct Device {
		enum class State {
			// There is no card here. (There probably used to be one,
			// but it got removed.) We don't insert a card before
			// we've actually probed it, ie., we know whether it
			// can be captured from at all, and what its name is.
			EMPTY,

			// This card is ready for capture, as far as we know.
			// (It could still be used by someone else; we don't know
			// until we try to open it.)
			READY,

			// We are trying to start capture from this card, but we are not
			// streaming yet. Note that this could in theory go on forever,
			// if the card is in use by some other process; in the UI,
			// we will show this state as “(busy)”.
			STARTING,

			// The card is capturing and sending data. If there's a fatal error,
			// it could go back to STARTING, or it could go to DEAD
			// (depending on the error).
			RUNNING,

			// The card is gone (e.g., unplugged). However, since there's
			// still a bus using it, we can't just remove the entry.
			// If the card comes back (ie., a new card is plugged in,
			// and we believe it has the same configuration), it could be
			// installed in place of this card, and then presumably be put
			// back into STARTING or RUNNING.
			DEAD
		} state = State::EMPTY;

		std::string address;  // E.g. “hw:0,0”.
		std::string name, info;
		unsigned num_channels;
		ALSAInput *input = nullptr;  // nullptr iff EMPTY or DEAD.

		// Whether the AudioMixer is interested in this card or not.
		// “Interested” could mean either of two things: Either it is part of
		// a bus mapping, or it is in the process of enumerating devices
		// (to show to the user). A card that is _not_ held can disappear
		// at any given time as a result of an error or hotplug event;
		// a card that is held will go to the DEAD state instead.
		bool held = false;
	};

	void init();

	// Get the list of all current devices. Note that this will implicitly mark
	// all of the returned devices as held, since the input mapping UI needs
	// some kind of stability when the user is to choose. Thus, when you are done
	// with the list and have set a new mapping, you must go through all the devices
	// you don't want and release them using release_device().
	std::vector<Device> get_devices();

	void hold_device(unsigned index);
	void release_device(unsigned index);  // Note: index is allowed to go out of bounds.

	// If device is held, start or restart capture. If device is not held,
	// stop capture if it isn't already.
	void reset_device(unsigned index);

	// Note: The card must be held. Returns OUTPUT_FREQUENCY if the card is in EMPTY or DEAD.
	unsigned get_capture_frequency(unsigned index);

	// Note: The card must be held.
	Device::State get_card_state(unsigned index);

	// Only for ALSAInput.
	void set_card_state(unsigned index, Device::State state);

	// Just a short form for taking <mu> and then moving the card to
	// EMPTY or DEAD state. Only for ALSAInput and for internal use.
	void free_card(unsigned index);

	// TODO: Add accessors and/or callbacks about changed state, so that
	// the UI actually stands a chance in using that information.

private:
	mutable std::mutex mu;
	std::vector<Device> devices;  // Under mu.
	std::vector<std::unique_ptr<ALSAInput>> inputs;  // Under mu, corresponds 1:1 to devices.

	// Keyed on device address (e.g. “hw:0,0”). If there's an entry here,
	// it means we already have a thread doing retries, so we shouldn't
	// start a new one.
	std::unordered_map<std::string, unsigned> add_device_tries_left;  // Under add_device_mutex.
	std::mutex add_device_mutex;

	static constexpr int num_retries = 10;

	void inotify_thread_func();
	void enumerate_devices();

	// Try to add an input at the given card/device. If it succeeds, return
	// synchronously. If not, fire off a background thread to try up to
	// <num_retries> times.
	void probe_device_with_retry(unsigned card_index, unsigned dev_index);
	void probe_device_retry_thread_func(unsigned card_index, unsigned dev_index);

	enum class ProbeResult {
		SUCCESS,
		DEFER,
		FAILURE
	};
	ProbeResult probe_device_once(unsigned card_index, unsigned dev_index);

	// Must be called with <mu> held. Will allocate a new entry if needed.
	// The returned entry will be set to READY state.
	unsigned find_free_device_index();

	friend class ALSAInput;
};

#endif  // !defined(_ALSA_INPUT_H)
