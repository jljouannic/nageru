// Rather simplistic benchmark of AudioMixer. Sets up a simple mapping
// with the default settings, feeds some white noise to the inputs and
// runs a while. Useful for e.g. profiling.

#include <stdio.h>
#include <stdint.h>
#include <vector>
#include <chrono>
#include "audio_mixer.h"
#include "timebase.h"

#define NUM_BENCHMARK_CARDS 4
#define NUM_WARMUP_FRAMES 100
#define NUM_BENCHMARK_FRAMES 1000
#define NUM_CHANNELS 8
#define NUM_SAMPLES 1024

using namespace std;
using namespace std::chrono;

// 16-bit samples, white noise at full volume.
uint8_t samples16[(NUM_SAMPLES * NUM_CHANNELS + 1024) * sizeof(uint16_t)];

// 24-bit samples, white noise at low volume (-48 dB).
uint8_t samples24[(NUM_SAMPLES * NUM_CHANNELS + 1024) * 3];

void callback(float level_lufs, float peak_db,
              std::vector<AudioMixer::BusLevel> bus_levels,
	      float global_level_lufs, float range_low_lufs, float range_high_lufs,
	      float final_makeup_gain_db,
	      float correlation)
{
	// Empty.
}

int main(void)
{
	for (unsigned i = 0; i < NUM_SAMPLES * NUM_CHANNELS + 1024; ++i) {
		samples16[i * 2] = rand() & 0xff;
		samples16[i * 2 + 1] = rand() & 0xff;

		samples24[i * 3] = rand() & 0xff;
		samples24[i * 3 + 1] = rand() & 0xff;
		samples24[i * 3 + 2] = 0;
	}
	AudioMixer mixer(NUM_BENCHMARK_CARDS);
	mixer.set_audio_level_callback(callback);

	InputMapping mapping;

	InputMapping::Bus bus1;
	bus1.device = DeviceSpec{InputSourceType::CAPTURE_CARD, 0};
	bus1.source_channel[0] = 0;
	bus1.source_channel[1] = 1;
	mapping.buses.push_back(bus1);

	InputMapping::Bus bus2;
	bus2.device = DeviceSpec{InputSourceType::CAPTURE_CARD, 3};
	bus2.source_channel[0] = 6;
	bus2.source_channel[1] = 4;
	mapping.buses.push_back(bus2);

	mixer.set_input_mapping(mapping);

	size_t out_samples = 0;

	steady_clock::time_point start, end;
	for (unsigned i = 0; i < NUM_WARMUP_FRAMES + NUM_BENCHMARK_FRAMES; ++i) {
		if (i == NUM_WARMUP_FRAMES) {
			start = steady_clock::now();
		}
		// Feed the inputs.
		for (unsigned card_index = 0; card_index < NUM_BENCHMARK_CARDS; ++card_index) {
			bmusb::AudioFormat audio_format;
			audio_format.bits_per_sample = card_index == 3 ? 24 : 16;
			audio_format.num_channels = NUM_CHANNELS;
			
			unsigned num_samples = NUM_SAMPLES + (rand() % 9) - 5;
			bool ok = mixer.add_audio(DeviceSpec{InputSourceType::CAPTURE_CARD, card_index},
				card_index == 3 ? samples24 : samples16, num_samples, audio_format,
				NUM_SAMPLES * TIMEBASE / OUTPUT_FREQUENCY);
			assert(ok);
		}

		double pts = double(i) * NUM_SAMPLES / OUTPUT_FREQUENCY;
		vector<float> output = mixer.get_output(pts, NUM_SAMPLES, ResamplingQueue::ADJUST_RATE);
		if (i >= NUM_WARMUP_FRAMES) {
			out_samples += output.size();
		}
	}
	end = steady_clock::now();

	double elapsed = duration<double>(end - start).count();
	double simulated = double(out_samples) / (OUTPUT_FREQUENCY * 2);
	printf("%ld samples produced in %.1f ms (%.1f%% CPU, %.1fx realtime).\n",
		out_samples, elapsed * 1e3, 100.0 * elapsed / simulated, simulated / elapsed);
}
