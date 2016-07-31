#ifndef _AUDIO_MIXER_H
#define _AUDIO_MIXER_H 1

// The audio mixer, dealing with extracting the right signals from
// each capture card, resampling signals so that they are in sync,
// processing them with effects (if desired), and then mixing them
// all together into one final audio signal.
//
// All operations on AudioMixer (except destruction) are thread-safe.
//
// TODO: There might be more audio stuff that should be moved here
// from Mixer.

#include <math.h>
#include <stdint.h>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

#include "bmusb/bmusb.h"
#include "db.h"
#include "defs.h"
#include "filter.h"
#include "resampling_queue.h"
#include "stereocompressor.h"

namespace bmusb {
struct AudioFormat;
}  // namespace bmusb

enum class InputSourceType { SILENCE, CAPTURE_CARD };

struct InputMapping {
	struct Bus {
		std::string name;
		InputSourceType input_source_type;
		unsigned input_source_index;
		int source_channel[2] { -1, -1 };  // Left and right. -1 = none.
	};

	std::vector<Bus> buses;
};

class AudioMixer {
public:
	AudioMixer(unsigned num_cards);
	void reset_card(unsigned card_index);

	// frame_length is in TIMEBASE units.
	void add_audio(unsigned card_index, const uint8_t *data, unsigned num_samples, bmusb::AudioFormat audio_format, int64_t frame_length);
	void add_silence(unsigned card_index, unsigned samples_per_frame, unsigned num_frames, int64_t frame_length);
	std::vector<float> get_output(double pts, unsigned num_samples, ResamplingQueue::RateAdjustmentPolicy rate_adjustment_policy);

	// See comments inside get_output().
	void set_current_loudness(double level_lufs) { loudness_lufs = level_lufs; }

	void set_fader_volume(unsigned bus_index, float level_db) { fader_volume_db[bus_index] = level_db; }
	std::vector<std::string> get_names() const;
	void set_name(unsigned card_index, const std::string &name);

	void set_input_mapping(const InputMapping &input_mapping);
	InputMapping get_input_mapping() const;

	void set_locut_cutoff(float cutoff_hz)
	{
		locut_cutoff_hz = cutoff_hz;
	}

	void set_locut_enabled(bool enabled)
	{
		locut_enabled = enabled;
	}

	bool get_locut_enabled() const
	{
		return locut_enabled;
	}

	float get_limiter_threshold_dbfs() const
	{
		return limiter_threshold_dbfs;
	}

	float get_compressor_threshold_dbfs() const
	{
		return compressor_threshold_dbfs;
	}

	void set_limiter_threshold_dbfs(float threshold_dbfs)
	{
		limiter_threshold_dbfs = threshold_dbfs;
	}

	void set_compressor_threshold_dbfs(float threshold_dbfs)
	{
		compressor_threshold_dbfs = threshold_dbfs;
	}

	void set_limiter_enabled(bool enabled)
	{
		limiter_enabled = enabled;
	}

	bool get_limiter_enabled() const
	{
		return limiter_enabled;
	}

	void set_compressor_enabled(bool enabled)
	{
		compressor_enabled = enabled;
	}

	bool get_compressor_enabled() const
	{
		return compressor_enabled;
	}

	void set_gain_staging_db(float gain_db)
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		level_compressor_enabled = false;
		gain_staging_db = gain_db;
	}

	float get_gain_staging_db() const
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		return gain_staging_db;
	}

	void set_gain_staging_auto(bool enabled)
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		level_compressor_enabled = enabled;
	}

	bool get_gain_staging_auto() const
	{
		std::unique_lock<std::mutex> lock(compressor_mutex);
		return level_compressor_enabled;
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

private:
	void find_sample_src_from_capture_card(const std::vector<float> *samples_card, unsigned card_index, int source_channel, const float **srcptr, unsigned *stride);
	void reset_card_mutex_held(unsigned card_index);

	unsigned num_cards;

	mutable std::mutex audio_mutex;

	struct CaptureCard {
		std::unique_ptr<ResamplingQueue> resampling_queue;
		int64_t next_local_pts = 0;
		std::string name;
		// Which channels we consider interesting (ie., are part of some input_mapping).
		std::set<unsigned> interesting_channels;
	};
	CaptureCard cards[MAX_CARDS];  // Under audio_mutex.

	StereoFilter locut;  // Default cutoff 120 Hz, 24 dB/oct.
	std::atomic<float> locut_cutoff_hz;
	std::atomic<bool> locut_enabled{true};

	// First compressor; takes us up to about -12 dBFS.
	mutable std::mutex compressor_mutex;
	StereoCompressor level_compressor;  // Under compressor_mutex. Used to set/override gain_staging_db if <level_compressor_enabled>.
	float gain_staging_db = 0.0f;  // Under compressor_mutex.
	bool level_compressor_enabled = true;  // Under compressor_mutex.

	static constexpr float ref_level_dbfs = -14.0f;  // Chosen so that we end up around 0 LU in practice.
	static constexpr float ref_level_lufs = -23.0f;  // 0 LU, more or less by definition.

	std::atomic<float> loudness_lufs{ref_level_lufs};

	StereoCompressor limiter;
	std::atomic<float> limiter_threshold_dbfs{ref_level_dbfs + 4.0f};   // 4 dB.
	std::atomic<bool> limiter_enabled{true};
	StereoCompressor compressor;
	std::atomic<float> compressor_threshold_dbfs{ref_level_dbfs - 12.0f};  // -12 dB.
	std::atomic<bool> compressor_enabled{true};

	double final_makeup_gain = 1.0;  // Under compressor_mutex. Read/write by the user. Note: Not in dB, we want the numeric precision so that we can change it slowly.
	bool final_makeup_gain_auto = true;  // Under compressor_mutex.

	InputMapping input_mapping;  // Under audio_mutex.
	std::atomic<float> fader_volume_db[MAX_BUSES] {{ 0.0f }};
};

#endif  // !defined(_AUDIO_MIXER_H)
