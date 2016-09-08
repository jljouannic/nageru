#include "alsa_input.h"
#include "audio_mixer.h"
#include "defs.h"

#include <sys/inotify.h>

#include <functional>
#include <unordered_map>

using namespace std;
using namespace std::placeholders;

namespace {

bool set_base_params(const char *device_name, snd_pcm_t *pcm_handle, snd_pcm_hw_params_t *hw_params, unsigned *sample_rate)
{
	int err;
	err = snd_pcm_hw_params_any(pcm_handle, hw_params);
	if (err < 0) {
		fprintf(stderr, "[%s] snd_pcm_hw_params_any(): %s\n", device_name, snd_strerror(err));
		return false;
	}
	err = snd_pcm_hw_params_set_access(pcm_handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED);
	if (err < 0) {
		fprintf(stderr, "[%s] snd_pcm_hw_params_set_access(): %s\n", device_name, snd_strerror(err));
		return false;
	}
	snd_pcm_format_mask_t *format_mask;
	snd_pcm_format_mask_alloca(&format_mask);
	snd_pcm_format_mask_set(format_mask, SND_PCM_FORMAT_S16_LE);
	snd_pcm_format_mask_set(format_mask, SND_PCM_FORMAT_S24_LE);
	snd_pcm_format_mask_set(format_mask, SND_PCM_FORMAT_S32_LE);
	err = snd_pcm_hw_params_set_format_mask(pcm_handle, hw_params, format_mask);
	if (err < 0) {
		fprintf(stderr, "[%s] snd_pcm_hw_params_set_format_mask(): %s\n", device_name, snd_strerror(err));
		return false;
	}
	err = snd_pcm_hw_params_set_rate_near(pcm_handle, hw_params, sample_rate, 0);
	if (err < 0) {
		fprintf(stderr, "[%s] snd_pcm_hw_params_set_rate_near(): %s\n", device_name, snd_strerror(err));
		return false;
	}
	return true;
}

}  // namespace

ALSAInput::ALSAInput(const char *device, unsigned sample_rate, unsigned num_channels, audio_callback_t audio_callback, ALSAPool *parent_pool, unsigned internal_dev_index)
	: device(device),
	  sample_rate(sample_rate),
	  num_channels(num_channels),
	  audio_callback(audio_callback),
	  parent_pool(parent_pool),
	  internal_dev_index(internal_dev_index)
{
	die_on_error(device, snd_pcm_open(&pcm_handle, device, SND_PCM_STREAM_CAPTURE, 0));

	// Set format.
	snd_pcm_hw_params_t *hw_params;
	snd_pcm_hw_params_alloca(&hw_params);
	if (!set_base_params(device, pcm_handle, hw_params, &sample_rate)) {
		exit(1);
	}

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
	parent_pool->set_card_state(internal_dev_index, ALSAPool::Device::State::STARTING);
	die_on_error("snd_pcm_start()", snd_pcm_start(pcm_handle));
	parent_pool->set_card_state(internal_dev_index, ALSAPool::Device::State::RUNNING);

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
	parent_pool->free_card(internal_dev_index);
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

ALSAPool::~ALSAPool()
{
	for (Device &device : devices) {
		if (device.input != nullptr) {
			device.input->stop_capture_thread();
		}
	}
}

std::vector<ALSAPool::Device> ALSAPool::get_devices()
{
	lock_guard<mutex> lock(mu);
	for (Device &device : devices) {
		device.held = true;
	}
	return devices;
}

void ALSAPool::hold_device(unsigned index)
{
	lock_guard<mutex> lock(mu);
	assert(index < devices.size());
	devices[index].held = true;
}

void ALSAPool::release_device(unsigned index)
{
	lock_guard<mutex> lock(mu);
	if (index < devices.size()) {
		devices[index].held = false;
	}
}

void ALSAPool::enumerate_devices()
{
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
		unique_ptr<snd_ctl_t, decltype(snd_ctl_close)*> ctl_closer(ctl, snd_ctl_close);

		// Enumerate all devices on this card.
		for (int dev_index = -1; snd_ctl_pcm_next_device(ctl, &dev_index) == 0 && dev_index >= 0; ) {
			probe_device_with_retry(card_index, dev_index);
		}
	}
}

void ALSAPool::probe_device_with_retry(unsigned card_index, unsigned dev_index)
{
	char address[256];
	snprintf(address, sizeof(address), "hw:%d,%d", card_index, dev_index);

	lock_guard<mutex> lock(add_device_mutex);
	if (add_device_tries_left.count(address)) {
		// Some thread is already busy retrying this,
		// so just reset its count.
		add_device_tries_left[address] = num_retries;
		return;
	}

	// Try (while still holding the lock) to add the device synchronously.
	ProbeResult result = probe_device_once(card_index, dev_index);
	if (result == ProbeResult::SUCCESS) {
		return;
	} else if (result == ProbeResult::FAILURE) {
		return;
	}
	assert(result == ProbeResult::DEFER);

	// Add failed for whatever reason (probably just that the device
	// isn't up yet. Set up a count so that nobody else starts a thread,
	// then start it ourselves.
	fprintf(stderr, "Trying %s again in one second...\n", address);
	add_device_tries_left[address] = num_retries;
	thread(&ALSAPool::probe_device_retry_thread_func, this, card_index, dev_index).detach();
}

void ALSAPool::probe_device_retry_thread_func(unsigned card_index, unsigned dev_index)
{
	char address[256];
	snprintf(address, sizeof(address), "hw:%d,%d", card_index, dev_index);

	for ( ;; ) {  // Termination condition within the loop.
		sleep(1);

		// See if there are any retries left.
		lock_guard<mutex> lock(add_device_mutex);
		if (!add_device_tries_left.count(address) ||
		    add_device_tries_left[address] == 0) {
			add_device_tries_left.erase(address);
			fprintf(stderr, "Giving up probe of %s.\n", address);
			return;
		}

		// Seemingly there were. Give it a try (we still hold the mutex).
		ProbeResult result = probe_device_once(card_index, dev_index);
		if (result == ProbeResult::SUCCESS) {
			add_device_tries_left.erase(address);
			fprintf(stderr, "Probe of %s succeeded.\n", address);
			return;
		} else if (result == ProbeResult::FAILURE || --add_device_tries_left[address] == 0) {
			add_device_tries_left.erase(address);
			fprintf(stderr, "Giving up probe of %s.\n", address);
			return;
		}

		// Failed again.
		assert(result == ProbeResult::DEFER);
		fprintf(stderr, "Trying %s again in one second (%d tries left)...\n",
			address, add_device_tries_left[address]);
	}
}

ALSAPool::ProbeResult ALSAPool::probe_device_once(unsigned card_index, unsigned dev_index)
{
	char address[256];
	snprintf(address, sizeof(address), "hw:%d", card_index);
	snd_ctl_t *ctl;
	int err = snd_ctl_open(&ctl, address, 0);
	if (err < 0) {
		printf("%s: %s\n", address, snd_strerror(err));
		return ALSAPool::ProbeResult::DEFER;
	}
	unique_ptr<snd_ctl_t, decltype(snd_ctl_close)*> ctl_closer(ctl, snd_ctl_close);

	snd_pcm_info_t *pcm_info;
	snd_pcm_info_alloca(&pcm_info);
	snd_pcm_info_set_device(pcm_info, dev_index);
	snd_pcm_info_set_subdevice(pcm_info, 0);
	snd_pcm_info_set_stream(pcm_info, SND_PCM_STREAM_CAPTURE);
	if (snd_ctl_pcm_info(ctl, pcm_info) < 0) {
		// Not available for capture.
		printf("%s: Not available for capture.\n", address);
		return ALSAPool::ProbeResult::DEFER;
	}

	snprintf(address, sizeof(address), "hw:%d,%d", card_index, dev_index);

	unsigned num_channels = 0;

	// Find all channel maps for this device, and pick out the one
	// with the most channels.
	snd_pcm_chmap_query_t **cmaps = snd_pcm_query_chmaps_from_hw(card_index, dev_index, 0, SND_PCM_STREAM_CAPTURE);
	if (cmaps != nullptr) {
		for (snd_pcm_chmap_query_t **ptr = cmaps; *ptr; ++ptr) {
			num_channels = max(num_channels, (*ptr)->map.channels);
		}
		snd_pcm_free_chmaps(cmaps);
	}
	if (num_channels == 0) {
		// Device had no channel maps. We need to open it to query.
		// TODO: Do this asynchronously.
		snd_pcm_t *pcm_handle;
		int err = snd_pcm_open(&pcm_handle, address, SND_PCM_STREAM_CAPTURE, 0);
		if (err < 0) {
			printf("%s: %s\n", address, snd_strerror(err));
			return ALSAPool::ProbeResult::DEFER;
		}
		snd_pcm_hw_params_t *hw_params;
		snd_pcm_hw_params_alloca(&hw_params);
		unsigned sample_rate;
		if (!set_base_params(address, pcm_handle, hw_params, &sample_rate)) {
			snd_pcm_close(pcm_handle);
			return ALSAPool::ProbeResult::DEFER;
		}
		err = snd_pcm_hw_params_get_channels_max(hw_params, &num_channels);
		if (err < 0) {
			fprintf(stderr, "[%s] snd_pcm_hw_params_get_channels_max(): %s\n",
				address, snd_strerror(err));
			snd_pcm_close(pcm_handle);
			return ALSAPool::ProbeResult::DEFER;
		}
		snd_pcm_close(pcm_handle);
	}

	if (num_channels == 0) {
		printf("%s: No channel maps with channels\n", address);
		return ALSAPool::ProbeResult::FAILURE;
	}

	snd_ctl_card_info_t *card_info;
	snd_ctl_card_info_alloca(&card_info);
	snd_ctl_card_info(ctl, card_info);

	lock_guard<mutex> lock(mu);
	unsigned internal_dev_index = find_free_device_index();
	devices[internal_dev_index].address = address;
	devices[internal_dev_index].name = snd_ctl_card_info_get_name(card_info);
	devices[internal_dev_index].info = snd_pcm_info_get_name(pcm_info);
	devices[internal_dev_index].num_channels = num_channels;
	devices[internal_dev_index].state = Device::State::READY;

	fprintf(stderr, "%s: Probed successfully.\n", address);

	return ALSAPool::ProbeResult::SUCCESS;
}

void ALSAPool::init()
{
	thread(&ALSAPool::inotify_thread_func, this).detach();
	enumerate_devices();
}

void ALSAPool::inotify_thread_func()
{
	int inotify_fd = inotify_init();
	if (inotify_fd == -1) {
		perror("inotify_init()");
		fprintf(stderr, "No hotplug of ALSA devices available.\n");
		return;
	}

	int watch_fd = inotify_add_watch(inotify_fd, "/dev/snd", IN_MOVE | IN_CREATE | IN_DELETE);
	if (watch_fd == -1) {
		perror("inotify_add_watch()");
		fprintf(stderr, "No hotplug of ALSA devices available.\n");
		close(inotify_fd);
		return;
	}

	int size = sizeof(inotify_event) + NAME_MAX + 1;
	unique_ptr<char[]> buf(new char[size]);
	for ( ;; ) {
		int ret = read(inotify_fd, buf.get(), size);
		if (ret < int(sizeof(inotify_event))) {
			fprintf(stderr, "inotify read unexpectedly returned %d, giving up hotplug of ALSA devices.\n",
				int(ret));
			close(watch_fd);
			close(inotify_fd);
			return;
		}

		for (int i = 0; i < ret; ) {
			const inotify_event *event = reinterpret_cast<const inotify_event *>(&buf[i]);
			i += sizeof(inotify_event) + event->len;

			if (event->mask & IN_Q_OVERFLOW) {
				fprintf(stderr, "WARNING: inotify overflowed, may lose ALSA hotplug events.\n");
				continue;
			}
			unsigned card, device;
			char type;
			if (sscanf(event->name, "pcmC%uD%u%c", &card, &device, &type) == 3 && type == 'c') {
				if (event->mask & (IN_MOVED_FROM | IN_DELETE)) {
					printf("Deleted capture device: Card %u, device %u\n", card, device);
					// TODO: Unplug.
				}
				if (event->mask & (IN_MOVED_TO | IN_CREATE)) {
					printf("Adding capture device: Card %u, device %u\n", card, device);
					probe_device_with_retry(card, device);
				}
			}
		}
	}
}

void ALSAPool::reset_device(unsigned index)
{
	lock_guard<mutex> lock(mu);
	Device *device = &devices[index];
	if (inputs[index] != nullptr) {
		inputs[index]->stop_capture_thread();
	}
	if (!device->held) {
		inputs[index].reset();
	} else {
		// TODO: Put on a background thread instead of locking?
		auto callback = bind(&AudioMixer::add_audio, global_audio_mixer, DeviceSpec{InputSourceType::ALSA_INPUT, index}, _1, _2, _3, _4);
		inputs[index].reset(new ALSAInput(device->address.c_str(), OUTPUT_FREQUENCY, device->num_channels, callback, this, index));
		inputs[index]->start_capture_thread();
	}
	device->input = inputs[index].get();
}

unsigned ALSAPool::get_capture_frequency(unsigned index)
{
	lock_guard<mutex> lock(mu);
	assert(devices[index].held);
	if (devices[index].input)
		return devices[index].input->get_sample_rate();
	else
		return OUTPUT_FREQUENCY;
}

ALSAPool::Device::State ALSAPool::get_card_state(unsigned index)
{
	lock_guard<mutex> lock(mu);
	assert(devices[index].held);
	return devices[index].state;
}

void ALSAPool::set_card_state(unsigned index, ALSAPool::Device::State state)
{
	lock_guard<mutex> lock(mu);
	devices[index].state = state;
}

unsigned ALSAPool::find_free_device_index()
{
	for (unsigned i = 0; i < devices.size(); ++i) {
		if (devices[i].state == Device::State::EMPTY) {
			devices[i].state = Device::State::READY;
			return i;
		}
	}
	Device new_dev;
	new_dev.state = Device::State::READY;
	devices.push_back(new_dev);
	inputs.emplace_back(nullptr);
	return devices.size() - 1;
}

void ALSAPool::free_card(unsigned index)
{
	lock_guard<mutex> lock(mu);
	if (devices[index].held) {
		devices[index].state = Device::State::DEAD;
	} else {
		devices[index].state = Device::State::EMPTY;
		inputs[index].reset();
	}
	while (!devices.empty() && devices.back().state == Device::State::EMPTY) {
		devices.pop_back();
		inputs.pop_back();
	}
}
