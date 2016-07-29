#include "audio_mixer.h"

#include <assert.h>
#include <endian.h>
#include <bmusb/bmusb.h>
#include <stdio.h>
#include <cmath>

#include "flags.h"
#include "timebase.h"

using namespace bmusb;
using namespace std;

namespace {

void convert_fixed24_to_fp32(float *dst, size_t out_channels, const uint8_t *src, size_t in_channels, size_t num_samples)
{
	assert(in_channels >= out_channels);
	for (size_t i = 0; i < num_samples; ++i) {
		for (size_t j = 0; j < out_channels; ++j) {
			uint32_t s1 = *src++;
			uint32_t s2 = *src++;
			uint32_t s3 = *src++;
			uint32_t s = s1 | (s1 << 8) | (s2 << 16) | (s3 << 24);
			dst[i * out_channels + j] = int(s) * (1.0f / 2147483648.0f);
		}
		src += 3 * (in_channels - out_channels);
	}
}

void convert_fixed32_to_fp32(float *dst, size_t out_channels, const uint8_t *src, size_t in_channels, size_t num_samples)
{
	assert(in_channels >= out_channels);
	for (size_t i = 0; i < num_samples; ++i) {
		for (size_t j = 0; j < out_channels; ++j) {
			int32_t s = le32toh(*(int32_t *)src);
			dst[i * out_channels + j] = s * (1.0f / 2147483648.0f);
			src += 4;
		}
		src += 4 * (in_channels - out_channels);
	}
}

}  // namespace

AudioMixer::AudioMixer(unsigned num_cards)
	: num_cards(num_cards),
	  level_compressor(OUTPUT_FREQUENCY),
	  limiter(OUTPUT_FREQUENCY),
	  compressor(OUTPUT_FREQUENCY)
{
	locut.init(FILTER_HPF, 2);

	set_locut_enabled(global_flags.locut_enabled);
	set_gain_staging_db(global_flags.initial_gain_staging_db);
	set_gain_staging_auto(global_flags.gain_staging_auto);
	set_compressor_enabled(global_flags.compressor_enabled);
	set_limiter_enabled(global_flags.limiter_enabled);
	set_final_makeup_gain_auto(global_flags.final_makeup_gain_auto);
}

void AudioMixer::reset_card(unsigned card_index)
{
	CaptureCard *card = &cards[card_index];

	unique_lock<mutex> lock(card->audio_mutex);
	card->resampling_queue.reset(new ResamplingQueue(card_index, OUTPUT_FREQUENCY, OUTPUT_FREQUENCY, 2));
	card->next_local_pts = 0;
}

void AudioMixer::add_audio(unsigned card_index, const uint8_t *data, unsigned num_samples, AudioFormat audio_format, int64_t frame_length)
{
	CaptureCard *card = &cards[card_index];

	// Convert the audio to stereo fp32.
	vector<float> audio;
	audio.resize(num_samples * 2);
	switch (audio_format.bits_per_sample) {
	case 0:
		assert(num_samples == 0);
		break;
	case 24:
		convert_fixed24_to_fp32(&audio[0], 2, data, audio_format.num_channels, num_samples);
		break;
	case 32:
		convert_fixed32_to_fp32(&audio[0], 2, data, audio_format.num_channels, num_samples);
		break;
	default:
		fprintf(stderr, "Cannot handle audio with %u bits per sample\n", audio_format.bits_per_sample);
		assert(false);
	}

	// Now add it.
	{
		unique_lock<mutex> lock(card->audio_mutex);

		int64_t local_pts = card->next_local_pts;
		card->resampling_queue->add_input_samples(local_pts / double(TIMEBASE), audio.data(), num_samples);
		card->next_local_pts = local_pts + frame_length;
	}
}

void AudioMixer::add_silence(unsigned card_index, unsigned samples_per_frame, unsigned num_frames, int64_t frame_length)
{
	CaptureCard *card = &cards[card_index];
	unique_lock<mutex> lock(card->audio_mutex);

	vector<float> silence(samples_per_frame * 2, 0.0f);
	for (unsigned i = 0; i < num_frames; ++i) {
		card->resampling_queue->add_input_samples(card->next_local_pts / double(TIMEBASE), silence.data(), samples_per_frame);
		// Note that if the format changed in the meantime, we have
		// no way of detecting that; we just have to assume the frame length
		// is always the same.
		card->next_local_pts += frame_length;
	}
}

vector<float> AudioMixer::get_output(double pts, unsigned num_samples, ResamplingQueue::RateAdjustmentPolicy rate_adjustment_policy)
{
	vector<float> samples_card;
	vector<float> samples_out;
	samples_out.resize(num_samples * 2);

	// TODO: Allow more flexible input mapping.
	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		samples_card.resize(num_samples * 2);
		{
			unique_lock<mutex> lock(cards[card_index].audio_mutex);
			cards[card_index].resampling_queue->get_output_samples(
				pts,
				&samples_card[0],
				num_samples,
				rate_adjustment_policy);
		}
		if (card_index == 0) {
			for (unsigned i = 0; i < num_samples * 2; ++i) {
				samples_out[i] = samples_card[i];
			}
		} else {
			for (unsigned i = 0; i < num_samples * 2; ++i) {
				samples_out[i] += samples_card[i];
			}
		}
	}

	// Cut away everything under 120 Hz (or whatever the cutoff is);
	// we don't need it for voice, and it will reduce headroom
	// and confuse the compressor. (In particular, any hums at 50 or 60 Hz
	// should be dampened.)
	if (locut_enabled) {
		locut.render(samples_out.data(), samples_out.size() / 2, locut_cutoff_hz * 2.0 * M_PI / OUTPUT_FREQUENCY, 0.5f);
	}

	{
		unique_lock<mutex> lock(compressor_mutex);

		// Apply a level compressor to get the general level right.
		// Basically, if it's over about -40 dBFS, we squeeze it down to that level
		// (or more precisely, near it, since we don't use infinite ratio),
		// then apply a makeup gain to get it to -14 dBFS. -14 dBFS is, of course,
		// entirely arbitrary, but from practical tests with speech, it seems to
		// put ut around -23 LUFS, so it's a reasonable starting point for later use.
		{
			if (level_compressor_enabled) {
				float threshold = 0.01f;   // -40 dBFS.
				float ratio = 20.0f;
				float attack_time = 0.5f;
				float release_time = 20.0f;
				float makeup_gain = pow(10.0f, (ref_level_dbfs - (-40.0f)) / 20.0f);  // +26 dB.
				level_compressor.process(samples_out.data(), samples_out.size() / 2, threshold, ratio, attack_time, release_time, makeup_gain);
				gain_staging_db = 20.0 * log10(level_compressor.get_attenuation() * makeup_gain);
			} else {
				// Just apply the gain we already had.
				float g = pow(10.0f, gain_staging_db / 20.0f);
				for (size_t i = 0; i < samples_out.size(); ++i) {
					samples_out[i] *= g;
				}
			}
		}

	#if 0
		printf("level=%f (%+5.2f dBFS) attenuation=%f (%+5.2f dB) end_result=%+5.2f dB\n",
			level_compressor.get_level(), 20.0 * log10(level_compressor.get_level()),
			level_compressor.get_attenuation(), 20.0 * log10(level_compressor.get_attenuation()),
			20.0 * log10(level_compressor.get_level() * level_compressor.get_attenuation() * makeup_gain));
	#endif

	//	float limiter_att, compressor_att;

		// The real compressor.
		if (compressor_enabled) {
			float threshold = pow(10.0f, compressor_threshold_dbfs / 20.0f);
			float ratio = 20.0f;
			float attack_time = 0.005f;
			float release_time = 0.040f;
			float makeup_gain = 2.0f;  // +6 dB.
			compressor.process(samples_out.data(), samples_out.size() / 2, threshold, ratio, attack_time, release_time, makeup_gain);
	//		compressor_att = compressor.get_attenuation();
		}

		// Finally a limiter at -4 dB (so, -10 dBFS) to take out the worst peaks only.
		// Note that since ratio is not infinite, we could go slightly higher than this.
		if (limiter_enabled) {
			float threshold = pow(10.0f, limiter_threshold_dbfs / 20.0f);
			float ratio = 30.0f;
			float attack_time = 0.0f;  // Instant.
			float release_time = 0.020f;
			float makeup_gain = 1.0f;  // 0 dB.
			limiter.process(samples_out.data(), samples_out.size() / 2, threshold, ratio, attack_time, release_time, makeup_gain);
	//		limiter_att = limiter.get_attenuation();
		}

	//	printf("limiter=%+5.1f  compressor=%+5.1f\n", 20.0*log10(limiter_att), 20.0*log10(compressor_att));
	}

	// At this point, we are most likely close to +0 LU, but all of our
	// measurements have been on raw sample values, not R128 values.
	// So we have a final makeup gain to get us to +0 LU; the gain
	// adjustments required should be relatively small, and also, the
	// offset shouldn't change much (only if the type of audio changes
	// significantly). Thus, we shoot for updating this value basically
	// “whenever we process buffers”, since the R128 calculation isn't exactly
	// something we get out per-sample.
	//
	// Note that there's a feedback loop here, so we choose a very slow filter
	// (half-time of 100 seconds).
	double target_loudness_factor, alpha;
	double loudness_lu = loudness_lufs - ref_level_lufs;
	double current_makeup_lu = 20.0f * log10(final_makeup_gain);
	target_loudness_factor = pow(10.0f, -loudness_lu / 20.0f);

	// If we're outside +/- 5 LU uncorrected, we don't count it as
	// a normal signal (probably silence) and don't change the
	// correction factor; just apply what we already have.
	if (fabs(loudness_lu - current_makeup_lu) >= 5.0 || !final_makeup_gain_auto) {
		alpha = 0.0;
	} else {
		// Formula adapted from
		// https://en.wikipedia.org/wiki/Low-pass_filter#Simple_infinite_impulse_response_filter.
		const double half_time_s = 100.0;
		const double fc_mul_2pi_delta_t = 1.0 / (half_time_s * OUTPUT_FREQUENCY);
		alpha = fc_mul_2pi_delta_t / (fc_mul_2pi_delta_t + 1.0);
	}

	{
		unique_lock<mutex> lock(compressor_mutex);
		double m = final_makeup_gain;
		for (size_t i = 0; i < samples_out.size(); i += 2) {
			samples_out[i + 0] *= m;
			samples_out[i + 1] *= m;
			m += (target_loudness_factor - m) * alpha;
		}
		final_makeup_gain = m;
	}

	return samples_out;
}
