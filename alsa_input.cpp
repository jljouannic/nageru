
#include "alsa_input.h"

using namespace std;

ALSAInput::ALSAInput(const char *device, unsigned sample_rate, unsigned num_channels, audio_callback_t audio_callback)
	: device(device), sample_rate(sample_rate), num_channels(num_channels), audio_callback(audio_callback)
{
	die_on_error(device, snd_pcm_open(&pcm_handle,device, SND_PCM_STREAM_CAPTURE, 0));

	// Set format.
	snd_pcm_hw_params_t *hw_params;
	snd_pcm_hw_params_alloca(&hw_params);
	die_on_error("snd_pcm_hw_params_any()", snd_pcm_hw_params_any(pcm_handle, hw_params));
	die_on_error("snd_pcm_hw_params_set_access()", snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED));
	snd_pcm_format_mask_t *format_mask;
	snd_pcm_format_mask_alloca(&format_mask);
	snd_pcm_format_mask_set(format_mask, SND_PCM_FORMAT_S16_LE);
	snd_pcm_format_mask_set(format_mask, SND_PCM_FORMAT_S24_LE);
	snd_pcm_format_mask_set(format_mask, SND_PCM_FORMAT_S32_LE);
	die_on_error("snd_pcm_hw_params_set_format()", snd_pcm_hw_params_set_format_mask(pcm_handle, hw_params, format_mask));
	die_on_error("snd_pcm_hw_params_set_rate_near()", snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, &sample_rate, 0));
	die_on_error("snd_pcm_hw_params_set_channels()", snd_pcm_hw_params_set_channels(pcm_handle, hw_params, num_channels));

	die_on_error("snd_pcm_hw_params_set_channels()", snd_pcm_hw_params_set_channels(pcm_handle, hw_params, num_channels));

	// Fragment size of 64 samples (about 1 ms at 48 kHz; a frame at 60
	// fps/48 kHz is 800 samples.) We ask for 64 such periods in our buffer
	// (~85 ms buffer); more than that, and our jitter is probably so high
	// that the resampling queue can't keep up anyway.
	// The entire thing with periods and such is a bit mysterious to me;
	// seemingly I can get 96 frames at a time with no problems even if
	// the period size is 64 frames. And if I set num_periods to e.g. 1,
	// I can't have a big buffer.
	num_periods = 16;
	int dir = 0;
	die_on_error("snd_pcm_hw_params_set_periods_near()", snd_pcm_hw_params_set_periods_near(pcm_handle, hw_params, &num_periods, &dir));
	period_size = 64;
	dir = 0;
	die_on_error("snd_pcm_hw_params_set_period_size_near()", snd_pcm_hw_params_set_period_size_near(pcm_handle, hw_params, &period_size, &dir));
	buffer_frames = 64 * 64;
	die_on_error("snd_pcm_hw_params_set_buffer_size_near()", snd_pcm_hw_params_set_buffer_size_near(pcm_handle, hw_params, &buffer_frames));
	die_on_error("snd_pcm_hw_params()", snd_pcm_hw_params(pcm_handle, hw_params));
	//snd_pcm_hw_params_free(hw_params);

	// Figure out which format the card actually chose.
	die_on_error("snd_pcm_hw_params_current()", snd_pcm_hw_params_current(pcm_handle, hw_params));
	snd_pcm_format_t chosen_format;
	die_on_error("snd_pcm_hw_params_get_format()", snd_pcm_hw_params_get_format(hw_params, &chosen_format));

	audio_format.num_channels = num_channels;
	audio_format.bits_per_sample = 0;
	switch (chosen_format) {
	case SND_PCM_FORMAT_S16_LE:
		audio_format.bits_per_sample = 16;
		break;
	case SND_PCM_FORMAT_S24_LE:
		audio_format.bits_per_sample = 24;
		break;
	case SND_PCM_FORMAT_S32_LE:
		audio_format.bits_per_sample = 32;
		break;
	default:
		assert(false);
	}
	//printf("num_periods=%u period_size=%u buffer_frames=%u sample_rate=%u bits_per_sample=%d\n",
	//	num_periods, unsigned(period_size), unsigned(buffer_frames), sample_rate, audio_format.bits_per_sample);

	buffer.reset(new uint8_t[buffer_frames * num_channels * audio_format.bits_per_sample / 8]);

	snd_pcm_sw_params_t *sw_params;
	snd_pcm_sw_params_alloca(&sw_params);
	die_on_error("snd_pcm_sw_params_current()", snd_pcm_sw_params_current(pcm_handle, sw_params));
	die_on_error("snd_pcm_sw_params_set_start_threshold", snd_pcm_sw_params_set_start_threshold(pcm_handle, sw_params, num_periods * period_size / 2));
	die_on_error("snd_pcm_sw_params()", snd_pcm_sw_params(pcm_handle, sw_params));

	die_on_error("snd_pcm_nonblock()", snd_pcm_nonblock(pcm_handle, 1));
	die_on_error("snd_pcm_prepare()", snd_pcm_prepare(pcm_handle));
}

ALSAInput::~ALSAInput()
{
	die_on_error("snd_pcm_close()", snd_pcm_close(pcm_handle));
}

void ALSAInput::start_capture_thread()
{
	should_quit = false;
	capture_thread = thread(&ALSAInput::capture_thread_func, this);
}

void ALSAInput::stop_capture_thread()
{
	should_quit = true;
	capture_thread.join();
}

void ALSAInput::capture_thread_func()
{
	die_on_error("snd_pcm_start()", snd_pcm_start(pcm_handle));
	uint64_t num_frames_output = 0;
	while (!should_quit) {
		int ret = snd_pcm_wait(pcm_handle, /*timeout=*/100);
		if (ret == 0) continue;  // Timeout.
		if (ret == -EPIPE) {
			fprintf(stderr, "[%s] ALSA overrun\n", device.c_str());
			snd_pcm_prepare(pcm_handle);
			snd_pcm_start(pcm_handle);
			continue;
		}
		die_on_error("snd_pcm_wait()", ret);

		snd_pcm_sframes_t frames = snd_pcm_readi(pcm_handle, buffer.get(), buffer_frames);
		if (frames == -EPIPE) {
			fprintf(stderr, "[%s] ALSA overrun\n", device.c_str());
			snd_pcm_prepare(pcm_handle);
			snd_pcm_start(pcm_handle);
			continue;
		}
		if (frames == 0) {
			fprintf(stderr, "snd_pcm_readi() returned 0\n");
			break;
		}
		die_on_error("snd_pcm_readi()", frames);

		const int64_t prev_pts = frames_to_pts(num_frames_output);
		const int64_t pts = frames_to_pts(num_frames_output + frames);
		bool success;
		do {
			if (should_quit) return;
			success = audio_callback(buffer.get(), frames, audio_format, pts - prev_pts);
		} while (!success);
		num_frames_output += frames;
	}
}

int64_t ALSAInput::frames_to_pts(uint64_t n) const
{
	return (n * TIMEBASE) / sample_rate;
}

void ALSAInput::die_on_error(const char *func_name, int err)
{
	if (err < 0) {
		fprintf(stderr, "[%s] %s: %s\n", device.c_str(), func_name, snd_strerror(err));
		exit(1);
	}
}

vector<ALSAInput::Device> ALSAInput::enumerate_devices()
{
	vector<Device> ret;

	// Enumerate all cards.
	for (int card_index = -1; snd_card_next(&card_index) == 0 && card_index >= 0; ) {
		char address[256];
		snprintf(address, sizeof(address), "hw:%d", card_index);

		snd_ctl_t *ctl;
		int err = snd_ctl_open(&ctl, address, 0);
		if (err < 0) {
			printf("%s: %s\n", address, snd_strerror(err));
			continue;
		}
		snd_ctl_card_info_t *card_info;
		snd_ctl_card_info_alloca(&card_info);
		snd_ctl_card_info(ctl, card_info);

		string card_name = snd_ctl_card_info_get_name(card_info);

		// Enumerate all devices on this card.
		for (int dev_index = -1; snd_ctl_pcm_next_device(ctl, &dev_index) == 0 && dev_index >= 0; ) {
			snd_pcm_info_t *pcm_info;
			snd_pcm_info_alloca(&pcm_info);
			snd_pcm_info_set_device(pcm_info, dev_index);
			snd_pcm_info_set_subdevice(pcm_info, 0);
			snd_pcm_info_set_stream(pcm_info, SND_PCM_STREAM_CAPTURE);
			if (snd_ctl_pcm_info(ctl, pcm_info) < 0) {
				// Not available for capture.
				continue;
			}

			snprintf(address, sizeof(address), "hw:%d,%d", card_index, dev_index);

			// Find all channel maps for this device, and pick out the one
			// with the most channels.
			snd_pcm_chmap_query_t **cmaps = snd_pcm_query_chmaps_from_hw(card_index, dev_index, 0, SND_PCM_STREAM_CAPTURE);
			unsigned num_channels = 0;
			for (snd_pcm_chmap_query_t **ptr = cmaps; *ptr; ++ptr) {
				num_channels = max(num_channels, (*ptr)->map.channels);
			}

			snd_pcm_free_chmaps(cmaps);

			if (num_channels == 0) {
				printf("%s: No channel maps with channels\n", address);
				continue;
			}

			Device dev;
			dev.address = address;
			dev.name = card_name;
			dev.info = snd_pcm_info_get_name(pcm_info);
			dev.num_channels = num_channels;

			ret.push_back(dev);
		}
		snd_ctl_close(ctl);
	}

	return ret;
}
