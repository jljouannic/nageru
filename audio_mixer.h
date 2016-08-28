#ifndef _AUDIO_MIXER_H
#define _AUDIO_MIXER_H 1

// The audio mixer, dealing with extracting the right signals from
// each capture card, resampling signals so that they are in sync,
// processing them with effects (if desired), and then mixing them
// all together into one final audio signal.
//
// All operations on AudioMixer (except destruction) are thread-safe.

#include <math.h>
#include <stdint.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>
#include <zita-resampler/resampler.h>

#include "alsa_input.h"
#include "bmusb/bmusb.h"
#include "correlation_measurer.h"
#include "db.h"
#include "defs.h"
#include "ebu_r128_proc.h"
#include "filter.h"
#include "resampling_queue.h"
#include "stereocompressor.h"

namespace bmusb {
struct AudioFormat;
}  // namespace bmusb

enum class InputSourceType { SILENCE, CAPTURE_CARD, ALSA_INPUT };
struct DeviceSpec {
	InputSourceType type;
	unsigned index;

	bool operator== (const DeviceSpec &other) const {
		return type == other.type && index == other.index;
	}

	bool operator< (const DeviceSpec &other) const {
		if (type != other.type)
			return type < other.type;
		return index < other.index;
	}
};
struct DeviceInfo {
	std::string name;
	unsigned num_channels;
};

enum EQBand {
	EQ_BAND_BASS = 0,
	EQ_BAND_MID,
	EQ_BAND_TREBLE,
	NUM_EQ_BANDS
};

static inline uint64_t DeviceSpec_to_key(const DeviceSpec &device_spec)
{
	return (uint64_t(device_spec.type) << 32) | device_spec.index;
}

static inline DeviceSpec key_to_DeviceSpec(uint64_t key)
{
	return DeviceSpec{ InputSourceType(key >> 32), unsigned(key & 0xffffffff) };
}

struct InputMapping {
	struct Bus {
		std::string name;
		DeviceSpec device;
		int source_channel[2] { -1, -1 };  // Left and right. -1 = none.
	};

	std::vector<Bus> buses;
};

class AudioMixer {
public:
	AudioMixer(unsigned num_cards);
	~AudioMixer();
	void reset_resampler(DeviceSpec device_spec);
	void reset_meters();

	// Add audio (or silence) to the given device's queue. Can return false if
	// the lock wasn't successfully taken; if so, you should simply try again.
	// (This is to avoid a deadlock where a card hangs on the mutex in add_audio()
	// while we are trying to shut it down from another thread that also holds
	// the mutex.) frame_length is in TIMEBASE units.
	bool add_audio(DeviceSpec device_spec, const uint8_t *data, unsigned num_samples, bmusb::AudioFormat audio_format, int64_t frame_length);
	bool add_silence(DeviceSpec device_spec, unsigned samples_per_frame, unsigned num_frames, int64_t frame_length);

	std::vector<float> get_output(double pts, unsigned num_samples, ResamplingQueue::RateAdjustmentPolicy rate_adjustment_policy);

	void set_fader_volume(unsigned bus_index, float level_db) { fader_volume_db[bus_index] = level_db; }
	std::map<DeviceSpec, DeviceInfo> get_devices() const;
	void set_name(DeviceSpec device_spec, const std::string &name);

	void set_input_mapping(const InputMapping &input_mapping);
	InputMapping get_input_mapping() const;

	void set_locut_cutoff(float cutoff_hz)
	{
		locut_cutoff_hz = cutoff_hz;
	}

	void set_locut_enabled(unsigned bus, bool enabled)
	{
		locut_enabled[bus] = enabled;
	}

	bool get_locut_enabled(unsigned bus)
	{
		return locut_enabled[bus];
	}

	void set_eq(unsigned bus_index, EQBand band, float db_gain)
	{
		assert(band >= 0 && band < NUM_EQ_BANDS);
		eq_level_db[bus_index][band] = db_gain;
	}

	float get_limiter_threshold_dbfs() const
	{
		return limiter_threshold_dbfs;
	}

	float get_compressor_threshold_dbfs(unsigned bus_index) const
	{
		return compressor_threshold_dbfs[bus_index];
	}

	void set_limiter_threshold_dbfs(float threshold_dbfs)
	{
		limiter_threshold_dbfs = threshold_dbfs;
	}

	void set_compressor_threshold_dbfs(unsigned bus_index, float threshold_dbfs)
	{
		compressor_threshold_dbfs[bus_index] = threshold_dbfs;
	}

	void set_limiter_enabled(bool enabled)
	{
		limiter_enabled = enabled;
	}

	bool get_limiter_enabled() const
	{
		return limiter_enabled;
	}

	void set_compressor_enabled(unsigned bus_index, bool enabled)
	{
		compressor_enabled[bus_index] = enabled;
	}

	bool get_compressor_enabled(unsigned bus_index) const
	{
		return compressor_enabled[bus_index];
	}

	void set_gain_staging_db(unsigned bus_index, float gain_db)
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		level_compressor_enabled[bus_index] = false;
		gain_staging_db[bus_index] = gain_db;
	}

	float get_gain_staging_db(unsigned bus_index) const
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		return gain_staging_db[bus_index];
	}

	void set_gain_staging_auto(unsigned bus_index, bool enabled)
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		level_compressor_enabled[bus_index] = enabled;
	}

	bool get_gain_staging_auto(unsigned bus_index) const
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		return level_compressor_enabled[bus_index];
	}

	void set_final_makeup_gain_db(float gain_db)
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		final_makeup_gain_auto = false;
		final_makeup_gain = from_db(gain_db);
	}

	float get_final_makeup_gain_db()
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		return to_db(final_makeup_gain);
	}

	void set_final_makeup_gain_auto(bool enabled)
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		final_makeup_gain_auto = enabled;
	}

	bool get_final_makeup_gain_auto() const
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		return final_makeup_gain_auto;
	}

	void reset_peak(unsigned bus_index);

	struct BusLevel {
		float current_level_dbfs[2];  // Digital peak of last frame, left and right.
		float peak_level_dbfs[2];  // Digital peak with hold, left and right.
		float historic_peak_dbfs;
		float gain_staging_db;
		float compressor_attenuation_db;  // A positive number; 0.0 for no attenuation.
	};

	typedef std::function<void(float level_lufs, float peak_db,
	                           std::vector<BusLevel> bus_levels,
	                           float global_level_lufs, float range_low_lufs, float range_high_lufs,
	                           float final_makeup_gain_db,
	                           float correlation)> audio_level_callback_t;
	void set_audio_level_callback(audio_level_callback_t callback)
	{
		audio_level_callback = callback;
	}

private:
	struct AudioDevice {
		std::unique_ptr<ResamplingQueue> resampling_queue;
		int64_t next_local_pts = 0;
		std::string name;
		unsigned capture_frequency = OUTPUT_FREQUENCY;
		// Which channels we consider interesting (ie., are part of some input_mapping).
		std::set<unsigned> interesting_channels;
		// Only used for ALSA cards, obviously.
		std::unique_ptr<ALSAInput> alsa_device;
	};
	AudioDevice *find_audio_device(DeviceSpec device_spec);

	void find_sample_src_from_device(const std::map<DeviceSpec, std::vector<float>> &samples_card, DeviceSpec device_spec, int source_channel, const float **srcptr, unsigned *stride);
	void fill_audio_bus(const std::map<DeviceSpec, std::vector<float>> &samples_card, const InputMapping::Bus &bus, unsigned num_samples, float *output);
	void reset_resampler_mutex_held(DeviceSpec device_spec);
	void reset_alsa_mutex_held(DeviceSpec device_spec);
	std::map<DeviceSpec, DeviceInfo> get_devices_mutex_held() const;
	void apply_eq(unsigned bus_index, std::vector<float> *samples_bus);
	void update_meters(const std::vector<float> &samples);
	void add_bus_to_master(unsigned bus_index, const std::vector<float> &samples_bus, std::vector<float> *samples_out);
	void measure_bus_levels(unsigned bus_index, const std::vector<float> &left, const std::vector<float> &right);
	void send_audio_level_callback();

	unsigned num_cards;

	mutable std::timed_mutex audio_mutex;

	AudioDevice video_cards[MAX_VIDEO_CARDS];  // Under audio_mutex.

	// TODO: Figure out a better way to unify these two, as they are sharing indexing.
	AudioDevice alsa_inputs[MAX_ALSA_CARDS];  // Under audio_mutex.
	std::vector<ALSAInput::Device> available_alsa_cards;

	std::atomic<float> locut_cutoff_hz;
	StereoFilter locut[MAX_BUSES];  // Default cutoff 120 Hz, 24 dB/oct.
	std::atomic<bool> locut_enabled[MAX_BUSES];
	StereoFilter eq[MAX_BUSES][NUM_EQ_BANDS];  // The one for EQBand::MID isn't actually used (see comments in apply_eq()).

	// First compressor; takes us up to about -12 dBFS.
	mutable std::mutex compressor_mutex;
	std::unique_ptr<StereoCompressor> level_compressor[MAX_BUSES];  // Under compressor_mutex. Used to set/override gain_staging_db if <level_compressor_enabled>.
	float gain_staging_db[MAX_BUSES];  // Under compressor_mutex.
	bool level_compressor_enabled[MAX_BUSES];  // Under compressor_mutex.

	static constexpr float ref_level_dbfs = -14.0f;  // Chosen so that we end up around 0 LU in practice.
	static constexpr float ref_level_lufs = -23.0f;  // 0 LU, more or less by definition.

	StereoCompressor limiter;
	std::atomic<float> limiter_threshold_dbfs{ref_level_dbfs + 4.0f};   // 4 dB.
	std::atomic<bool> limiter_enabled{true};
	std::unique_ptr<StereoCompressor> compressor[MAX_BUSES];
	std::atomic<float> compressor_threshold_dbfs[MAX_BUSES];
	std::atomic<bool> compressor_enabled[MAX_BUSES];

	// Note: The values here are not in dB.
	struct PeakHistory {
		float current_level = 0.0f;  // Peak of the last frame.
		float historic_peak = 0.0f;  // Highest peak since last reset; no falloff.
		float current_peak = 0.0f;  // Current peak of the peak meter.
		float last_peak = 0.0f;
		float age_seconds = 0.0f;   // Time since "last_peak" was set.
	};
	PeakHistory peak_history[MAX_BUSES][2];  // Separate for each channel. Under audio_mutex.

	double final_makeup_gain = 1.0;  // Under compressor_mutex. Read/write by the user. Note: Not in dB, we want the numeric precision so that we can change it slowly.
	bool final_makeup_gain_auto = true;  // Under compressor_mutex.

	InputMapping input_mapping;  // Under audio_mutex.
	std::atomic<float> fader_volume_db[MAX_BUSES] {{ 0.0f }};
	float last_fader_volume_db[MAX_BUSES] { 0.0f };  // Under audio_mutex.
	std::atomic<float> eq_level_db[MAX_BUSES][NUM_EQ_BANDS] {{{ 0.0f }}};

	audio_level_callback_t audio_level_callback = nullptr;
	mutable std::mutex audio_measure_mutex;
	Ebu_r128_proc r128;  // Under audio_measure_mutex.
	CorrelationMeasurer correlation;  // Under audio_measure_mutex.
	Resampler peak_resampler;  // Under audio_measure_mutex.
	std::atomic<float> peak{0.0f};
};

#endif  // !defined(_AUDIO_MIXER_H)
