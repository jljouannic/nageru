#ifndef _FAKE_CAPTURE_H
#define _FAKE_CAPTURE_H 1

#include <stdint.h>
#include <functional>
#include <string>

#include "bmusb/bmusb.h"

class FakeCapture : public CaptureInterface
{
public:
	FakeCapture(int card_index);
	~FakeCapture();

	// CaptureInterface.
	void set_video_frame_allocator(FrameAllocator *allocator) override
	{
		video_frame_allocator = allocator;
		if (owned_video_frame_allocator.get() != allocator) {
			owned_video_frame_allocator.reset();
		}
	}

	FrameAllocator *get_video_frame_allocator() override
	{
		return video_frame_allocator;
	}

	// Does not take ownership.
	void set_audio_frame_allocator(FrameAllocator *allocator) override
	{
		audio_frame_allocator = allocator;
		if (owned_audio_frame_allocator.get() != allocator) {
			owned_audio_frame_allocator.reset();
		}
	}

	FrameAllocator *get_audio_frame_allocator() override
	{
		return audio_frame_allocator;
	}

	void set_frame_callback(frame_callback_t callback) override
	{
		frame_callback = callback;
	}

	void set_dequeue_thread_callbacks(std::function<void()> init, std::function<void()> cleanup) override
	{
		dequeue_init_callback = init;
		dequeue_cleanup_callback = cleanup;
		has_dequeue_callbacks = true;
	}

	std::string get_description() const override
	{
		return description;
	}

	void configure_card() override;
	void start_bm_capture() override;
	void stop_dequeue_thread() override;

	std::map<uint32_t, VideoMode> get_available_video_modes() const override;
	void set_video_mode(uint32_t video_mode_id) override;
	uint32_t get_current_video_mode() const override { return 0; }

	std::map<uint32_t, std::string> get_available_video_inputs() const override;
	void set_video_input(uint32_t video_input_id) override;
	uint32_t get_current_video_input() const override { return 0; }

	std::map<uint32_t, std::string> get_available_audio_inputs() const override;
	void set_audio_input(uint32_t audio_input_id) override;
	uint32_t get_current_audio_input() const override { return 0; }

private:
	void producer_thread_func();

	uint8_t y, cb, cr;

	bool has_dequeue_callbacks = false;
	std::function<void()> dequeue_init_callback = nullptr;
	std::function<void()> dequeue_cleanup_callback = nullptr;

	FrameAllocator *video_frame_allocator = nullptr;
	FrameAllocator *audio_frame_allocator = nullptr;
	std::unique_ptr<FrameAllocator> owned_video_frame_allocator;
	std::unique_ptr<FrameAllocator> owned_audio_frame_allocator;
	frame_callback_t frame_callback = nullptr;

	std::string description;

	std::atomic<bool> producer_thread_should_quit{false};
	std::thread producer_thread;
};

#endif  // !defined(_FAKE_CAPTURE_H)
