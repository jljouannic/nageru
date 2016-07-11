// A fake capture device that sends single-color frames at a given rate.
// Mostly useful for testing themes without actually hooking up capture devices.

#include "fake_capture.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cstddef>

#include "bmusb/bmusb.h"
#include "defs.h"

#define FRAME_SIZE (8 << 20)  // 8 MB.
#define FAKE_FPS 25  // Must be an integer.

// Pure-color inputs: Red, green, blue, white.
#define NUM_COLORS 4
constexpr uint8_t ys[NUM_COLORS] = { 81, 145, 41, 235 };
constexpr uint8_t cbs[NUM_COLORS] = { 90, 54, 240, 128 };
constexpr uint8_t crs[NUM_COLORS] = { 240, 34, 110, 128 };

using namespace std;

namespace {

// TODO: SSE2-optimize (or at least write full int64s) if speed becomes a problem.

void memset2(uint8_t *s, const uint8_t c[2], size_t n)
{
	for (size_t i = 0; i < n; ++i) {
		*s++ = c[0];
		*s++ = c[1];
	}
}

void memset4(uint8_t *s, const uint8_t c[4], size_t n)
{
	for (size_t i = 0; i < n; ++i) {
		*s++ = c[0];
		*s++ = c[1];
		*s++ = c[2];
		*s++ = c[3];
	}
}

}  // namespace

FakeCapture::FakeCapture(int card_index)
{
	char buf[256];
	snprintf(buf, sizeof(buf), "Fake card %d", card_index + 1);
	description = buf;

	y = ys[card_index % NUM_COLORS];
	cb = cbs[card_index % NUM_COLORS];
	cr = crs[card_index % NUM_COLORS];
}

FakeCapture::~FakeCapture()
{
	if (has_dequeue_callbacks) {
		dequeue_cleanup_callback();
	}
}

void FakeCapture::configure_card()
{
	if (video_frame_allocator == nullptr) {
		owned_video_frame_allocator.reset(new MallocFrameAllocator(FRAME_SIZE, NUM_QUEUED_VIDEO_FRAMES));
		set_video_frame_allocator(owned_video_frame_allocator.get());
	}
	if (audio_frame_allocator == nullptr) {
		owned_audio_frame_allocator.reset(new MallocFrameAllocator(65536, NUM_QUEUED_AUDIO_FRAMES));
		set_audio_frame_allocator(owned_audio_frame_allocator.get());
	}
}

void FakeCapture::start_bm_capture()
{
	producer_thread_should_quit = false;
	producer_thread = thread(&FakeCapture::producer_thread_func, this);
}

void FakeCapture::stop_dequeue_thread()
{
	producer_thread_should_quit = true;
	producer_thread.join();
}
	
std::map<uint32_t, VideoMode> FakeCapture::get_available_video_modes() const
{
	VideoMode mode;

	char buf[256];
	snprintf(buf, sizeof(buf), "%dx%d", WIDTH, HEIGHT);
	mode.name = buf;
	
	mode.autodetect = false;
	mode.width = WIDTH;
	mode.height = HEIGHT;
	mode.frame_rate_num = FAKE_FPS;
	mode.frame_rate_den = 1;
	mode.interlaced = false;

	return {{ 0, mode }};
}

std::map<uint32_t, std::string> FakeCapture::get_available_video_inputs() const
{
	return {{ 0, "Fake video input (single color)" }};
}

std::map<uint32_t, std::string> FakeCapture::get_available_audio_inputs() const
{
	return {{ 0, "Fake audio input (silence)" }};
}

void FakeCapture::set_video_mode(uint32_t video_mode_id)
{
	assert(video_mode_id == 0);
}

void FakeCapture::set_video_input(uint32_t video_input_id)
{
	assert(video_input_id == 0);
}

void FakeCapture::set_audio_input(uint32_t audio_input_id)
{
	assert(audio_input_id == 0);
}

void FakeCapture::producer_thread_func()
{
	uint16_t timecode = 0;

	if (has_dequeue_callbacks) {
		dequeue_init_callback();
	}
	while (!producer_thread_should_quit) {
		usleep(1000000 / FAKE_FPS);  // Rather approximate frame rate.

		if (producer_thread_should_quit) break;

		VideoFormat video_format;
		video_format.width = WIDTH;
		video_format.height = HEIGHT;
		video_format.frame_rate_nom = FAKE_FPS;
		video_format.frame_rate_den = 1;
		video_format.has_signal = true;

		FrameAllocator::Frame video_frame = video_frame_allocator->alloc_frame();
		if (video_frame.data != nullptr) {
			assert(video_frame.size >= WIDTH * HEIGHT * 2);
			if (video_frame.interleaved) {
				uint8_t cbcr[] = { cb, cr };
				memset2(video_frame.data, cbcr, WIDTH * HEIGHT / 2);
				memset(video_frame.data2, y, WIDTH * HEIGHT);
			} else {
				uint8_t ycbcr[] = { y, cb, y, cr };
				memset4(video_frame.data, ycbcr, WIDTH * HEIGHT / 2);
			}
			video_frame.len = WIDTH * HEIGHT * 2;
		}

		AudioFormat audio_format;
		audio_format.bits_per_sample = 32;
		audio_format.num_channels = 2;

		FrameAllocator::Frame audio_frame = audio_frame_allocator->alloc_frame();
		if (audio_frame.data != nullptr) {
			assert(audio_frame.size >= 2 * sizeof(uint32_t) * OUTPUT_FREQUENCY / FAKE_FPS);
			audio_frame.len = 2 * sizeof(uint32_t) * OUTPUT_FREQUENCY / FAKE_FPS;
			memset(audio_frame.data, 0, audio_frame.len);
		}

		frame_callback(timecode++,
			       video_frame, 0, video_format,
			       audio_frame, 0, audio_format);
	}
	if (has_dequeue_callbacks) {
		dequeue_cleanup_callback();
	}
}
