#include "audio_mixer.h"

#include <assert.h>
#include <endian.h>
#include <bmusb/bmusb.h>
#include <stdio.h>
#include <endian.h>
#include <cmath>

#include "db.h"
#include "flags.h"
#include "timebase.h"

using namespace bmusb;
using namespace std;
using namespace std::placeholders;

namespace {

// TODO: If these prove to be a bottleneck, they can be SSSE3-optimized
// (usually including multiple channels at a time).

void convert_fixed16_to_fp32(float *dst, size_t out_channel, size_t out_num_channels,
                             const uint8_t *src, size_t in_channel, size_t in_num_channels,
                             size_t num_samples)
{
	assert(in_channel < in_num_channels);
	assert(out_channel < out_num_channels);
	src += in_channel * 2;
	dst += out_channel;

	for (size_t i = 0; i < num_samples; ++i) {
		int16_t s = le16toh(*(int16_t *)src);
		*dst = s * (1.0f / 32768.0f);

		src += 2 * in_num_channels;
		dst += out_num_channels;
	}
}

void convert_fixed24_to_fp32(float *dst, size_t out_channel, size_t out_num_channels,
                             const uint8_t *src, size_t in_channel, size_t in_num_channels,
                             size_t num_samples)
{
	assert(in_channel < in_num_channels);
	assert(out_channel < out_num_channels);
	src += in_channel * 3;
	dst += out_channel;

	for (size_t i = 0; i < num_samples; ++i) {
		uint32_t s1 = src[0];
		uint32_t s2 = src[1];
		uint32_t s3 = src[2];
		uint32_t s = s1 | (s1 << 8) | (s2 << 16) | (s3 << 24);
		*dst = int(s) * (1.0f / 2147483648.0f);

		src += 3 * in_num_channels;
		dst += out_num_channels;
	}
}

void convert_fixed32_to_fp32(float *dst, size_t out_channel, size_t out_num_channels,
                             const uint8_t *src, size_t in_channel, size_t in_num_channels,
                             size_t num_samples)
{
	assert(in_channel < in_num_channels);
	assert(out_channel < out_num_channels);
	src += in_channel * 4;
	dst += out_channel;

	for (size_t i = 0; i < num_samples; ++i) {
		int32_t s = le32toh(*(int32_t *)src);
		*dst = s * (1.0f / 2147483648.0f);

		src += 4 * in_num_channels;
		dst += out_num_channels;
	}
}

float find_peak(const float *samples, size_t num_samples)
{
	float m = fabs(samples[0]);
	for (size_t i = 1; i < num_samples; ++i) {
		m = max(m, fabs(samples[i]));
	}
	return m;
}

void deinterleave_samples(const vector<float> &in, vector<float> *out_l, vector<float> *out_r)
{
	size_t num_samples = in.size() / 2;
	out_l->resize(num_samples);
	out_r->resize(num_samples);

	const float *inptr = in.data();
	float *lptr = &(*out_l)[0];
	float *rptr = &(*out_r)[0];
	for (size_t i = 0; i < num_samples; ++i) {
		*lptr++ = *inptr++;
		*rptr++ = *inptr++;
	}
}

}  // namespace

AudioMixer::AudioMixer(unsigned num_cards)
	: num_cards(num_cards),
	  limiter(OUTPUT_FREQUENCY),
	  correlation(OUTPUT_FREQUENCY)
{
	for (unsigned bus_index = 0; bus_index < MAX_BUSES; ++bus_index) {
		locut[bus_index].init(FILTER_HPF, 2);
		locut_enabled[bus_index] = global_flags.locut_enabled;
		gain_staging_db[bus_index] = global_flags.initial_gain_staging_db;
		compressor[bus_index].reset(new StereoCompressor(OUTPUT_FREQUENCY));
		compressor_threshold_dbfs[bus_index] = ref_level_dbfs - 12.0f;  // -12 dB.
		compressor_enabled[bus_index] = global_flags.compressor_enabled;
		level_compressor[bus_index].reset(new StereoCompressor(OUTPUT_FREQUENCY));
		level_compressor_enabled[bus_index] = global_flags.gain_staging_auto;
	}
	set_limiter_enabled(global_flags.limiter_enabled);
	set_final_makeup_gain_auto(global_flags.final_makeup_gain_auto);

	// Generate a very simple, default input mapping.
	InputMapping::Bus input;
	input.name = "Main";
	input.device.type = InputSourceType::CAPTURE_CARD;
	input.device.index = 0;
	input.source_channel[0] = 0;
	input.source_channel[1] = 1;

	InputMapping new_input_mapping;
	new_input_mapping.buses.push_back(input);
	set_input_mapping(new_input_mapping);

	// Look for ALSA cards.
	available_alsa_cards = ALSAInput::enumerate_devices();

	r128.init(2, OUTPUT_FREQUENCY);
	r128.integr_start();

	// hlen=16 is pretty low quality, but we use quite a bit of CPU otherwise,
	// and there's a limit to how important the peak meter is.
	peak_resampler.setup(OUTPUT_FREQUENCY, OUTPUT_FREQUENCY * 4, /*num_channels=*/2, /*hlen=*/16, /*frel=*/1.0);
}

AudioMixer::~AudioMixer()
{
	for (unsigned card_index = 0; card_index < available_alsa_cards.size(); ++card_index) {
		const AudioDevice &device = alsa_inputs[card_index];
		if (device.alsa_device != nullptr) {
			device.alsa_device->stop_capture_thread();
		}
	}
}


void AudioMixer::reset_resampler(DeviceSpec device_spec)
{
	lock_guard<timed_mutex> lock(audio_mutex);
	reset_resampler_mutex_held(device_spec);
}

void AudioMixer::reset_resampler_mutex_held(DeviceSpec device_spec)
{
	AudioDevice *device = find_audio_device(device_spec);

	if (device->interesting_channels.empty()) {
		device->resampling_queue.reset();
	} else {
		// TODO: ResamplingQueue should probably take the full device spec.
		// (It's only used for console output, though.)
		device->resampling_queue.reset(new ResamplingQueue(device_spec.index, device->capture_frequency, OUTPUT_FREQUENCY, device->interesting_channels.size()));
	}
	device->next_local_pts = 0;
}

void AudioMixer::reset_alsa_mutex_held(DeviceSpec device_spec)
{
	assert(device_spec.type == InputSourceType::ALSA_INPUT);
	unsigned card_index = device_spec.index;
	AudioDevice *device = find_audio_device(device_spec);

	if (device->alsa_device != nullptr) {
		device->alsa_device->stop_capture_thread();
	}
	if (device->interesting_channels.empty()) {
		device->alsa_device.reset();
	} else {
		const ALSAInput::Device &alsa_dev = available_alsa_cards[card_index];
		device->alsa_device.reset(new ALSAInput(alsa_dev.address.c_str(), OUTPUT_FREQUENCY, alsa_dev.num_channels, bind(&AudioMixer::add_audio, this, device_spec, _1, _2, _3, _4)));
		device->capture_frequency = device->alsa_device->get_sample_rate();
		device->alsa_device->start_capture_thread();
	}
}

bool AudioMixer::add_audio(DeviceSpec device_spec, const uint8_t *data, unsigned num_samples, AudioFormat audio_format, int64_t frame_length)
{
	AudioDevice *device = find_audio_device(device_spec);

	unique_lock<timed_mutex> lock(audio_mutex, defer_lock);
	if (!lock.try_lock_for(chrono::milliseconds(10))) {
		return false;
	}
	if (device->resampling_queue == nullptr) {
		// No buses use this device; throw it away.
		return true;
	}

	unsigned num_channels = device->interesting_channels.size();
	assert(num_channels > 0);

	// Convert the audio to fp32.
	vector<float> audio;
	audio.resize(num_samples * num_channels);
	unsigned channel_index = 0;
	for (auto channel_it = device->interesting_channels.cbegin(); channel_it != device->interesting_channels.end(); ++channel_it, ++channel_index) {
		switch (audio_format.bits_per_sample) {
		case 0:
			assert(num_samples == 0);
			break;
		case 16:
			convert_fixed16_to_fp32(&audio[0], channel_index, num_channels, data, *channel_it, audio_format.num_channels, num_samples);
			break;
		case 24:
			convert_fixed24_to_fp32(&audio[0], channel_index, num_channels, data, *channel_it, audio_format.num_channels, num_samples);
			break;
		case 32:
			convert_fixed32_to_fp32(&audio[0], channel_index, num_channels, data, *channel_it, audio_format.num_channels, num_samples);
			break;
		default:
			fprintf(stderr, "Cannot handle audio with %u bits per sample\n", audio_format.bits_per_sample);
			assert(false);
		}
	}

	// Now add it.
	int64_t local_pts = device->next_local_pts;
	device->resampling_queue->add_input_samples(local_pts / double(TIMEBASE), audio.data(), num_samples);
	device->next_local_pts = local_pts + frame_length;
	return true;
}

bool AudioMixer::add_silence(DeviceSpec device_spec, unsigned samples_per_frame, unsigned num_frames, int64_t frame_length)
{
	AudioDevice *device = find_audio_device(device_spec);

	unique_lock<timed_mutex> lock(audio_mutex, defer_lock);
	if (!lock.try_lock_for(chrono::milliseconds(10))) {
		return false;
	}
	if (device->resampling_queue == nullptr) {
		// No buses use this device; throw it away.
		return true;
	}

	unsigned num_channels = device->interesting_channels.size();
	assert(num_channels > 0);

	vector<float> silence(samples_per_frame * num_channels, 0.0f);
	for (unsigned i = 0; i < num_frames; ++i) {
		device->resampling_queue->add_input_samples(device->next_local_pts / double(TIMEBASE), silence.data(), samples_per_frame);
		// Note that if the format changed in the meantime, we have
		// no way of detecting that; we just have to assume the frame length
		// is always the same.
		device->next_local_pts += frame_length;
	}
	return true;
}

AudioMixer::AudioDevice *AudioMixer::find_audio_device(DeviceSpec device)
{
	switch (device.type) {
	case InputSourceType::CAPTURE_CARD:
		return &video_cards[device.index];
	case InputSourceType::ALSA_INPUT:
		return &alsa_inputs[device.index];
	case InputSourceType::SILENCE:
	default:
		assert(false);
	}
	return nullptr;
}

// Get a pointer to the given channel from the given device.
// The channel must be picked out earlier and resampled.
void AudioMixer::find_sample_src_from_device(const map<DeviceSpec, vector<float>> &samples_card, DeviceSpec device_spec, int source_channel, const float **srcptr, unsigned *stride)
{
	static float zero = 0.0f;
	if (source_channel == -1 || device_spec.type == InputSourceType::SILENCE) {
		*srcptr = &zero;
		*stride = 0;
		return;
	}
	AudioDevice *device = find_audio_device(device_spec);
	assert(device->interesting_channels.count(source_channel) != 0);
	unsigned channel_index = 0;
	for (int channel : device->interesting_channels) {
		if (channel == source_channel) break;
		++channel_index;
	}
	assert(channel_index < device->interesting_channels.size());
	const auto it = samples_card.find(device_spec);
	assert(it != samples_card.end());
	*srcptr = &(it->second)[channel_index];
	*stride = device->interesting_channels.size();
}

// TODO: Can be SSSE3-optimized if need be.
void AudioMixer::fill_audio_bus(const map<DeviceSpec, vector<float>> &samples_card, const InputMapping::Bus &bus, unsigned num_samples, float *output)
{
	if (bus.device.type == InputSourceType::SILENCE) {
		memset(output, 0, num_samples * sizeof(*output));
	} else {
		assert(bus.device.type == InputSourceType::CAPTURE_CARD ||
		       bus.device.type == InputSourceType::ALSA_INPUT);
		const float *lsrc, *rsrc;
		unsigned lstride, rstride;
		float *dptr = output;
		find_sample_src_from_device(samples_card, bus.device, bus.source_channel[0], &lsrc, &lstride);
		find_sample_src_from_device(samples_card, bus.device, bus.source_channel[1], &rsrc, &rstride);
		for (unsigned i = 0; i < num_samples; ++i) {
			*dptr++ = *lsrc;
			*dptr++ = *rsrc;
			lsrc += lstride;
			rsrc += rstride;
		}
	}
}

vector<float> AudioMixer::get_output(double pts, unsigned num_samples, ResamplingQueue::RateAdjustmentPolicy rate_adjustment_policy)
{
	map<DeviceSpec, vector<float>> samples_card;
	vector<float> samples_bus;

	lock_guard<timed_mutex> lock(audio_mutex);

	// Pick out all the interesting channels from all the cards.
	// TODO: If the card has been hotswapped, the number of channels
	// might have changed; if so, we need to do some sort of remapping
	// to silence.
	for (const auto &spec_and_info : get_devices_mutex_held()) {
		const DeviceSpec &device_spec = spec_and_info.first;
		AudioDevice *device = find_audio_device(device_spec);
		if (!device->interesting_channels.empty()) {
			samples_card[device_spec].resize(num_samples * device->interesting_channels.size());
			device->resampling_queue->get_output_samples(
				pts,
				&samples_card[device_spec][0],
				num_samples,
				rate_adjustment_policy);
		}
	}

	vector<float> samples_out, left, right;
	samples_out.resize(num_samples * 2);
	samples_bus.resize(num_samples * 2);
	for (unsigned bus_index = 0; bus_index < input_mapping.buses.size(); ++bus_index) {
		fill_audio_bus(samples_card, input_mapping.buses[bus_index], num_samples, &samples_bus[0]);

		// Cut away everything under 120 Hz (or whatever the cutoff is);
		// we don't need it for voice, and it will reduce headroom
		// and confuse the compressor. (In particular, any hums at 50 or 60 Hz
		// should be dampened.)
		if (locut_enabled[bus_index]) {
			locut[bus_index].render(samples_bus.data(), samples_bus.size() / 2, locut_cutoff_hz * 2.0 * M_PI / OUTPUT_FREQUENCY, 0.5f);
		}

		{
			lock_guard<mutex> lock(compressor_mutex);

			// Apply a level compressor to get the general level right.
			// Basically, if it's over about -40 dBFS, we squeeze it down to that level
			// (or more precisely, near it, since we don't use infinite ratio),
			// then apply a makeup gain to get it to -14 dBFS. -14 dBFS is, of course,
			// entirely arbitrary, but from practical tests with speech, it seems to
			// put ut around -23 LUFS, so it's a reasonable starting point for later use.
			if (level_compressor_enabled[bus_index]) {
				float threshold = 0.01f;   // -40 dBFS.
				float ratio = 20.0f;
				float attack_time = 0.5f;
				float release_time = 20.0f;
				float makeup_gain = from_db(ref_level_dbfs - (-40.0f));  // +26 dB.
				level_compressor[bus_index]->process(samples_bus.data(), samples_bus.size() / 2, threshold, ratio, attack_time, release_time, makeup_gain);
				gain_staging_db[bus_index] = to_db(level_compressor[bus_index]->get_attenuation() * makeup_gain);
			} else {
				// Just apply the gain we already had.
				float g = from_db(gain_staging_db[bus_index]);
				for (size_t i = 0; i < samples_bus.size(); ++i) {
					samples_bus[i] *= g;
				}
			}

#if 0
			printf("level=%f (%+5.2f dBFS) attenuation=%f (%+5.2f dB) end_result=%+5.2f dB\n",
				level_compressor.get_level(), to_db(level_compressor.get_level()),
				level_compressor.get_attenuation(), to_db(level_compressor.get_attenuation()),
				to_db(level_compressor.get_level() * level_compressor.get_attenuation() * makeup_gain));
#endif

			// The real compressor.
			if (compressor_enabled[bus_index]) {
				float threshold = from_db(compressor_threshold_dbfs[bus_index]);
				float ratio = 20.0f;
				float attack_time = 0.005f;
				float release_time = 0.040f;
				float makeup_gain = 2.0f;  // +6 dB.
				compressor[bus_index]->process(samples_bus.data(), samples_bus.size() / 2, threshold, ratio, attack_time, release_time, makeup_gain);
		//		compressor_att = compressor.get_attenuation();
			}
		}

		// TODO: We should measure post-fader.
		deinterleave_samples(samples_bus, &left, &right);
		measure_bus_levels(bus_index, left, right);

		float volume = from_db(fader_volume_db[bus_index]);
		if (bus_index == 0) {
			for (unsigned i = 0; i < num_samples * 2; ++i) {
				samples_out[i] = samples_bus[i] * volume;
			}
		} else {
			for (unsigned i = 0; i < num_samples * 2; ++i) {
				samples_out[i] += samples_bus[i] * volume;
			}
		}
	}

	{
		lock_guard<mutex> lock(compressor_mutex);

		// Finally a limiter at -4 dB (so, -10 dBFS) to take out the worst peaks only.
		// Note that since ratio is not infinite, we could go slightly higher than this.
		if (limiter_enabled) {
			float threshold = from_db(limiter_threshold_dbfs);
			float ratio = 30.0f;
			float attack_time = 0.0f;  // Instant.
			float release_time = 0.020f;
			float makeup_gain = 1.0f;  // 0 dB.
			limiter.process(samples_out.data(), samples_out.size() / 2, threshold, ratio, attack_time, release_time, makeup_gain);
	//		limiter_att = limiter.get_attenuation();
		}

	//	printf("limiter=%+5.1f  compressor=%+5.1f\n", to_db(limiter_att), to_db(compressor_att));
	}

	// At this point, we are most likely close to +0 LU (at least if the
	// faders sum to 0 dB and the compressors are on), but all of our
	// measurements have been on raw sample values, not R128 values.
	// So we have a final makeup gain to get us to +0 LU; the gain
	// adjustments required should be relatively small, and also, the
	// offset shouldn't change much (only if the type of audio changes
	// significantly). Thus, we shoot for updating this value basically
	// “whenever we process buffers”, since the R128 calculation isn't exactly
	// something we get out per-sample.
	//
	// Note that there's a feedback loop here, so we choose a very slow filter
	// (half-time of 30 seconds).
	double target_loudness_factor, alpha;
	double loudness_lu = r128.loudness_M() - ref_level_lufs;
	double current_makeup_lu = to_db(final_makeup_gain);
	target_loudness_factor = final_makeup_gain * from_db(-loudness_lu);

	// If we're outside +/- 5 LU uncorrected, we don't count it as
	// a normal signal (probably silence) and don't change the
	// correction factor; just apply what we already have.
	if (fabs(loudness_lu - current_makeup_lu) >= 5.0 || !final_makeup_gain_auto) {
		alpha = 0.0;
	} else {
		// Formula adapted from
		// https://en.wikipedia.org/wiki/Low-pass_filter#Simple_infinite_impulse_response_filter.
		const double half_time_s = 30.0;
		const double fc_mul_2pi_delta_t = 1.0 / (half_time_s * OUTPUT_FREQUENCY);
		alpha = fc_mul_2pi_delta_t / (fc_mul_2pi_delta_t + 1.0);
	}

	{
		lock_guard<mutex> lock(compressor_mutex);
		double m = final_makeup_gain;
		for (size_t i = 0; i < samples_out.size(); i += 2) {
			samples_out[i + 0] *= m;
			samples_out[i + 1] *= m;
			m += (target_loudness_factor - m) * alpha;
		}
		final_makeup_gain = m;
	}

	update_meters(samples_out);

	return samples_out;
}

void AudioMixer::measure_bus_levels(unsigned bus_index, const vector<float> &left, const vector<float> &right)
{
	const float *ptrs[] = { left.data(), right.data() };
	{
		lock_guard<mutex> lock(audio_measure_mutex);
		bus_r128[bus_index]->process(left.size(), const_cast<float **>(ptrs));
	}
}

void AudioMixer::update_meters(const vector<float> &samples)
{
	// Upsample 4x to find interpolated peak.
	peak_resampler.inp_data = const_cast<float *>(samples.data());
	peak_resampler.inp_count = samples.size() / 2;

	vector<float> interpolated_samples;
	interpolated_samples.resize(samples.size());
	{
		lock_guard<mutex> lock(audio_measure_mutex);

		while (peak_resampler.inp_count > 0) {  // About four iterations.
			peak_resampler.out_data = &interpolated_samples[0];
			peak_resampler.out_count = interpolated_samples.size() / 2;
			peak_resampler.process();
			size_t out_stereo_samples = interpolated_samples.size() / 2 - peak_resampler.out_count;
			peak = max<float>(peak, find_peak(interpolated_samples.data(), out_stereo_samples * 2));
			peak_resampler.out_data = nullptr;
		}
	}

	// Find R128 levels and L/R correlation.
	vector<float> left, right;
	deinterleave_samples(samples, &left, &right);
	float *ptrs[] = { left.data(), right.data() };
	{
		lock_guard<mutex> lock(audio_measure_mutex);
		r128.process(left.size(), ptrs);
		correlation.process_samples(samples);
	}

	send_audio_level_callback();
}

void AudioMixer::reset_meters()
{
	lock_guard<mutex> lock(audio_measure_mutex);
	peak_resampler.reset();
	peak = 0.0f;
	r128.reset();
	r128.integr_start();
	correlation.reset();
}

void AudioMixer::send_audio_level_callback()
{
	if (audio_level_callback == nullptr) {
		return;
	}

	lock_guard<mutex> lock(audio_measure_mutex);
	double loudness_s = r128.loudness_S();
	double loudness_i = r128.integrated();
	double loudness_range_low = r128.range_min();
	double loudness_range_high = r128.range_max();

	vector<BusLevel> bus_levels;
	bus_levels.resize(input_mapping.buses.size());
	for (unsigned bus_index = 0; bus_index < bus_r128.size(); ++bus_index) {
		bus_levels[bus_index].loudness_lufs = bus_r128[bus_index]->loudness_S();
		bus_levels[bus_index].gain_staging_db = gain_staging_db[bus_index];
	}

	audio_level_callback(loudness_s, to_db(peak), bus_levels,
		loudness_i, loudness_range_low, loudness_range_high,
		to_db(final_makeup_gain),
		correlation.get_correlation());
}

map<DeviceSpec, DeviceInfo> AudioMixer::get_devices() const
{
	lock_guard<timed_mutex> lock(audio_mutex);
	return get_devices_mutex_held();
}

map<DeviceSpec, DeviceInfo> AudioMixer::get_devices_mutex_held() const
{
	map<DeviceSpec, DeviceInfo> devices;
	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		const DeviceSpec spec{ InputSourceType::CAPTURE_CARD, card_index };
		const AudioDevice *device = &video_cards[card_index];
		DeviceInfo info;
		info.name = device->name;
		info.num_channels = 8;  // FIXME: This is wrong for fake cards.
		devices.insert(make_pair(spec, info));
	}
	for (unsigned card_index = 0; card_index < available_alsa_cards.size(); ++card_index) {
		const DeviceSpec spec{ InputSourceType::ALSA_INPUT, card_index };
		const ALSAInput::Device &device = available_alsa_cards[card_index];
		DeviceInfo info;
		info.name = device.name + " (" + device.info + ")";
		info.num_channels = device.num_channels;
		devices.insert(make_pair(spec, info));
	}
	return devices;
}

void AudioMixer::set_name(DeviceSpec device_spec, const string &name)
{
	AudioDevice *device = find_audio_device(device_spec);

	lock_guard<timed_mutex> lock(audio_mutex);
	device->name = name;
}

void AudioMixer::set_input_mapping(const InputMapping &new_input_mapping)
{
	lock_guard<timed_mutex> lock(audio_mutex);

	map<DeviceSpec, set<unsigned>> interesting_channels;
	for (const InputMapping::Bus &bus : new_input_mapping.buses) {
		if (bus.device.type == InputSourceType::CAPTURE_CARD ||
		    bus.device.type == InputSourceType::ALSA_INPUT) {
			for (unsigned channel = 0; channel < 2; ++channel) {
				if (bus.source_channel[channel] != -1) {
					interesting_channels[bus.device].insert(bus.source_channel[channel]);
				}
			}
		}
	}

	// Reset resamplers for all cards that don't have the exact same state as before.
	for (const auto &spec_and_info : get_devices_mutex_held()) {
		const DeviceSpec &device_spec = spec_and_info.first;
		AudioDevice *device = find_audio_device(device_spec);
		if (device->interesting_channels != interesting_channels[device_spec]) {
			device->interesting_channels = interesting_channels[device_spec];
			if (device_spec.type == InputSourceType::ALSA_INPUT) {
				reset_alsa_mutex_held(device_spec);
			}
			reset_resampler_mutex_held(device_spec);
		}
	}

	{
		lock_guard<mutex> lock(audio_measure_mutex);
		bus_r128.resize(new_input_mapping.buses.size());
		for (unsigned bus_index = 0; bus_index < bus_r128.size(); ++bus_index) {
			if (bus_r128[bus_index] == nullptr) {
				bus_r128[bus_index].reset(new Ebu_r128_proc);
			}
			bus_r128[bus_index]->init(2, OUTPUT_FREQUENCY);
		}
	}

	input_mapping = new_input_mapping;
}

InputMapping AudioMixer::get_input_mapping() const
{
	lock_guard<timed_mutex> lock(audio_mutex);
	return input_mapping;
}
