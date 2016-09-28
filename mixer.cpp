#undef Success

#include "mixer.h"

#include <assert.h>
#include <endian.h>
#include <epoxy/egl.h>
#include <movit/effect_chain.h>
#include <movit/effect_util.h>
#include <movit/flat_input.h>
#include <movit/image_format.h>
#include <movit/init.h>
#include <movit/resource_pool.h>
#include <movit/util.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <time.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "bmusb/bmusb.h"
#include "bmusb/fake_capture.h"
#include "context.h"
#include "decklink_capture.h"
#include "defs.h"
#include "disk_space_estimator.h"
#include "flags.h"
#include "video_encoder.h"
#include "pbo_frame_allocator.h"
#include "ref_counted_gl_sync.h"
#include "timebase.h"

class QOpenGLContext;

using namespace movit;
using namespace std;
using namespace std::chrono;
using namespace std::placeholders;
using namespace bmusb;

Mixer *global_mixer = nullptr;
bool uses_mlock = false;

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

void insert_new_frame(RefCountedFrame frame, unsigned field_num, bool interlaced, unsigned card_index, InputState *input_state)
{
	if (interlaced) {
		for (unsigned frame_num = FRAME_HISTORY_LENGTH; frame_num --> 1; ) {  // :-)
			input_state->buffered_frames[card_index][frame_num] =
				input_state->buffered_frames[card_index][frame_num - 1];
		}
		input_state->buffered_frames[card_index][0] = { frame, field_num };
	} else {
		for (unsigned frame_num = 0; frame_num < FRAME_HISTORY_LENGTH; ++frame_num) {
			input_state->buffered_frames[card_index][frame_num] = { frame, field_num };
		}
	}
}

}  // namespace

void QueueLengthPolicy::update_policy(int queue_length)
{
	if (queue_length < 0) {  // Starvation.
		if (been_at_safe_point_since_last_starvation && safe_queue_length < 5) {
			++safe_queue_length;
			fprintf(stderr, "Card %u: Starvation, increasing safe limit to %u frames\n",
				card_index, safe_queue_length);
		}
		frames_with_at_least_one = 0;
		been_at_safe_point_since_last_starvation = false;
		return;
	}
	if (queue_length > 0) {
		if (queue_length >= int(safe_queue_length)) {
			been_at_safe_point_since_last_starvation = true;
		}
		if (++frames_with_at_least_one >= 1000 && safe_queue_length > 0) {
			--safe_queue_length;
			fprintf(stderr, "Card %u: Spare frames for more than 1000 frames, reducing safe limit to %u frames\n",
				card_index, safe_queue_length);
			frames_with_at_least_one = 0;
		}
	} else {
		frames_with_at_least_one = 0;
	}
}

Mixer::Mixer(const QSurfaceFormat &format, unsigned num_cards)
	: httpd(),
	  num_cards(num_cards),
	  mixer_surface(create_surface(format)),
	  h264_encoder_surface(create_surface(format)),
	  correlation(OUTPUT_FREQUENCY),
	  level_compressor(OUTPUT_FREQUENCY),
	  limiter(OUTPUT_FREQUENCY),
	  compressor(OUTPUT_FREQUENCY)
{
	CHECK(init_movit(MOVIT_SHADER_DIR, MOVIT_DEBUG_OFF));
	check_error();

	// Since we allow non-bouncing 4:2:2 YCbCrInputs, effective subpixel precision
	// will be halved when sampling them, and we need to compensate here.
	movit_texel_subpixel_precision /= 2.0;

	resource_pool.reset(new ResourcePool);
	theme.reset(new Theme(global_flags.theme_filename, global_flags.theme_dirs, resource_pool.get(), num_cards));
	for (unsigned i = 0; i < NUM_OUTPUTS; ++i) {
		output_channel[i].parent = this;
		output_channel[i].channel = i;
	}

	ImageFormat inout_format;
	inout_format.color_space = COLORSPACE_sRGB;
	inout_format.gamma_curve = GAMMA_sRGB;

	// Display chain; shows the live output produced by the main chain (its RGBA version).
	display_chain.reset(new EffectChain(WIDTH, HEIGHT, resource_pool.get()));
	check_error();
	display_input = new FlatInput(inout_format, FORMAT_RGB, GL_UNSIGNED_BYTE, WIDTH, HEIGHT);  // FIXME: GL_UNSIGNED_BYTE is really wrong.
	display_chain->add_input(display_input);
	display_chain->add_output(inout_format, OUTPUT_ALPHA_FORMAT_POSTMULTIPLIED);
	display_chain->set_dither_bits(0);  // Don't bother.
	display_chain->finalize();

	video_encoder.reset(new VideoEncoder(resource_pool.get(), h264_encoder_surface, global_flags.va_display, WIDTH, HEIGHT, &httpd, global_disk_space_estimator));

	// Start listening for clients only once VideoEncoder has written its header, if any.
	httpd.start(9095);

	// First try initializing the then PCI devices, then USB, then
	// fill up with fake cards until we have the desired number of cards.
	unsigned num_pci_devices = 0;
	unsigned card_index = 0;

	{
		IDeckLinkIterator *decklink_iterator = CreateDeckLinkIteratorInstance();
		if (decklink_iterator != nullptr) {
			for ( ; card_index < num_cards; ++card_index) {
				IDeckLink *decklink;
				if (decklink_iterator->Next(&decklink) != S_OK) {
					break;
				}

				configure_card(card_index, new DeckLinkCapture(decklink, card_index), /*is_fake_capture=*/false);
				++num_pci_devices;
			}
			decklink_iterator->Release();
			fprintf(stderr, "Found %u DeckLink PCI card(s).\n", num_pci_devices);
		} else {
			fprintf(stderr, "DeckLink drivers not found. Probing for USB cards only.\n");
		}
	}
	unsigned num_usb_devices = BMUSBCapture::num_cards();
	for (unsigned usb_card_index = 0; usb_card_index < num_usb_devices && card_index < num_cards; ++usb_card_index, ++card_index) {
		BMUSBCapture *capture = new BMUSBCapture(usb_card_index);
		capture->set_card_disconnected_callback(bind(&Mixer::bm_hotplug_remove, this, card_index));
		configure_card(card_index, capture, /*is_fake_capture=*/false);
	}
	fprintf(stderr, "Found %u USB card(s).\n", num_usb_devices);

	unsigned num_fake_cards = 0;
	for ( ; card_index < num_cards; ++card_index, ++num_fake_cards) {
		FakeCapture *capture = new FakeCapture(WIDTH, HEIGHT, FAKE_FPS, OUTPUT_FREQUENCY, card_index, global_flags.fake_cards_audio);
		configure_card(card_index, capture, /*is_fake_capture=*/true);
	}

	if (num_fake_cards > 0) {
		fprintf(stderr, "Initialized %u fake cards.\n", num_fake_cards);
	}

	BMUSBCapture::set_card_connected_callback(bind(&Mixer::bm_hotplug_add, this, _1));
	BMUSBCapture::start_bm_thread();

	for (card_index = 0; card_index < num_cards; ++card_index) {
		cards[card_index].queue_length_policy.reset(card_index);
		cards[card_index].capture->start_bm_capture();
	}

	// Set up stuff for NV12 conversion.

	// Cb/Cr shader.
	string cbcr_vert_shader =
		"#version 130 \n"
		" \n"
		"in vec2 position; \n"
		"in vec2 texcoord; \n"
		"out vec2 tc0; \n"
		"uniform vec2 foo_chroma_offset_0; \n"
		" \n"
		"void main() \n"
		"{ \n"
		"    // The result of glOrtho(0.0, 1.0, 0.0, 1.0, 0.0, 1.0) is: \n"
		"    // \n"
		"    //   2.000  0.000  0.000 -1.000 \n"
		"    //   0.000  2.000  0.000 -1.000 \n"
		"    //   0.000  0.000 -2.000 -1.000 \n"
		"    //   0.000  0.000  0.000  1.000 \n"
		"    gl_Position = vec4(2.0 * position.x - 1.0, 2.0 * position.y - 1.0, -1.0, 1.0); \n"
		"    vec2 flipped_tc = texcoord; \n"
		"    tc0 = flipped_tc + foo_chroma_offset_0; \n"
		"} \n";
	string cbcr_frag_shader =
		"#version 130 \n"
		"in vec2 tc0; \n"
		"uniform sampler2D cbcr_tex; \n"
		"out vec4 FragColor; \n"
		"void main() { \n"
		"    FragColor = texture(cbcr_tex, tc0); \n"
		"} \n";
	vector<string> frag_shader_outputs;
	cbcr_program_num = resource_pool->compile_glsl_program(cbcr_vert_shader, cbcr_frag_shader, frag_shader_outputs);

	float vertices[] = {
		0.0f, 2.0f,
		0.0f, 0.0f,
		2.0f, 0.0f
	};
	cbcr_vbo = generate_vbo(2, GL_FLOAT, sizeof(vertices), vertices);
	cbcr_position_attribute_index = glGetAttribLocation(cbcr_program_num, "position");
	cbcr_texcoord_attribute_index = glGetAttribLocation(cbcr_program_num, "texcoord");

	r128.init(2, OUTPUT_FREQUENCY);
	r128.integr_start();

	locut.init(FILTER_HPF, 2);

	set_locut_enabled(global_flags.locut_enabled);
	set_gain_staging_db(global_flags.initial_gain_staging_db);
	set_gain_staging_auto(global_flags.gain_staging_auto);
	set_compressor_enabled(global_flags.compressor_enabled);
	set_limiter_enabled(global_flags.limiter_enabled);
	set_final_makeup_gain_auto(global_flags.final_makeup_gain_auto);

	// hlen=16 is pretty low quality, but we use quite a bit of CPU otherwise,
	// and there's a limit to how important the peak meter is.
	peak_resampler.setup(OUTPUT_FREQUENCY, OUTPUT_FREQUENCY * 4, /*num_channels=*/2, /*hlen=*/16, /*frel=*/1.0);

	if (global_flags.enable_alsa_output) {
		alsa.reset(new ALSAOutput(OUTPUT_FREQUENCY, /*num_channels=*/2));
	}
}

Mixer::~Mixer()
{
	resource_pool->release_glsl_program(cbcr_program_num);
	glDeleteBuffers(1, &cbcr_vbo);
	BMUSBCapture::stop_bm_thread();

	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		{
			unique_lock<mutex> lock(bmusb_mutex);
			cards[card_index].should_quit = true;  // Unblock thread.
			cards[card_index].new_frames_changed.notify_all();
		}
		cards[card_index].capture->stop_dequeue_thread();
	}

	video_encoder.reset(nullptr);
}

void Mixer::configure_card(unsigned card_index, CaptureInterface *capture, bool is_fake_capture)
{
	printf("Configuring card %d...\n", card_index);

	CaptureCard *card = &cards[card_index];
	if (card->capture != nullptr) {
		card->capture->stop_dequeue_thread();
		delete card->capture;
	}
	card->capture = capture;
	card->is_fake_capture = is_fake_capture;
	card->capture->set_frame_callback(bind(&Mixer::bm_frame, this, card_index, _1, _2, _3, _4, _5, _6, _7));
	if (card->frame_allocator == nullptr) {
		card->frame_allocator.reset(new PBOFrameAllocator(8 << 20, WIDTH, HEIGHT));  // 8 MB.
	}
	card->capture->set_video_frame_allocator(card->frame_allocator.get());
	if (card->surface == nullptr) {
		card->surface = create_surface_with_same_format(mixer_surface);
	}
	{
		unique_lock<mutex> lock(cards[card_index].audio_mutex);
		card->resampling_queue.reset(new ResamplingQueue(card_index, OUTPUT_FREQUENCY, OUTPUT_FREQUENCY, 2));
	}
	while (!card->new_frames.empty()) card->new_frames.pop();
	card->fractional_samples = 0;
	card->last_timecode = -1;
	card->next_local_pts = 0;
	card->capture->configure_card();
}


namespace {

int unwrap_timecode(uint16_t current_wrapped, int last)
{
	uint16_t last_wrapped = last & 0xffff;
	if (current_wrapped > last_wrapped) {
		return (last & ~0xffff) | current_wrapped;
	} else {
		return 0x10000 + ((last & ~0xffff) | current_wrapped);
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

void Mixer::bm_frame(unsigned card_index, uint16_t timecode,
                     FrameAllocator::Frame video_frame, size_t video_offset, VideoFormat video_format,
		     FrameAllocator::Frame audio_frame, size_t audio_offset, AudioFormat audio_format)
{
	CaptureCard *card = &cards[card_index];

	if (is_mode_scanning[card_index]) {
		if (video_format.has_signal) {
			// Found a stable signal, so stop scanning.
			is_mode_scanning[card_index] = false;
		} else {
			static constexpr double switch_time_s = 0.5;  // Should be enough time for the signal to stabilize.
			steady_clock::time_point now = steady_clock::now();
			double sec_since_last_switch = duration<double>(steady_clock::now() - last_mode_scan_change[card_index]).count();
			if (sec_since_last_switch > switch_time_s) {
				// It isn't this mode; try the next one.
				mode_scanlist_index[card_index]++;
				mode_scanlist_index[card_index] %= mode_scanlist[card_index].size();
				cards[card_index].capture->set_video_mode(mode_scanlist[card_index][mode_scanlist_index[card_index]]);
				last_mode_scan_change[card_index] = now;
			}
		}
	}

	int64_t frame_length = int64_t(TIMEBASE) * video_format.frame_rate_den / video_format.frame_rate_nom;
	assert(frame_length > 0);

	size_t num_samples = (audio_frame.len > audio_offset) ? (audio_frame.len - audio_offset) / audio_format.num_channels / (audio_format.bits_per_sample / 8) : 0;
	if (num_samples > OUTPUT_FREQUENCY / 10) {
		printf("Card %d: Dropping frame with implausible audio length (len=%d, offset=%d) [timecode=0x%04x video_len=%d video_offset=%d video_format=%x)\n",
			card_index, int(audio_frame.len), int(audio_offset),
			timecode, int(video_frame.len), int(video_offset), video_format.id);
		if (video_frame.owner) {
			video_frame.owner->release_frame(video_frame);
		}
		if (audio_frame.owner) {
			audio_frame.owner->release_frame(audio_frame);
		}
		return;
	}

	int64_t local_pts = card->next_local_pts;
	int dropped_frames = 0;
	if (card->last_timecode != -1) {
		dropped_frames = unwrap_timecode(timecode, card->last_timecode) - card->last_timecode - 1;
	}

	// Convert the audio to stereo fp32 and add it.
	vector<float> audio;
	audio.resize(num_samples * 2);
	switch (audio_format.bits_per_sample) {
	case 0:
		assert(num_samples == 0);
		break;
	case 24:
		convert_fixed24_to_fp32(&audio[0], 2, audio_frame.data + audio_offset, audio_format.num_channels, num_samples);
		break;
	case 32:
		convert_fixed32_to_fp32(&audio[0], 2, audio_frame.data + audio_offset, audio_format.num_channels, num_samples);
		break;
	default:
		fprintf(stderr, "Cannot handle audio with %u bits per sample\n", audio_format.bits_per_sample);
		assert(false);
	}

	// Add the audio.
	{
		unique_lock<mutex> lock(card->audio_mutex);

		// Number of samples per frame if we need to insert silence.
		// (Could be nonintegral, but resampling will save us then.)
		int silence_samples = OUTPUT_FREQUENCY * video_format.frame_rate_den / video_format.frame_rate_nom;

		if (dropped_frames > MAX_FPS * 2) {
			fprintf(stderr, "Card %d lost more than two seconds (or time code jumping around; from 0x%04x to 0x%04x), resetting resampler\n",
				card_index, card->last_timecode, timecode);
			card->resampling_queue.reset(new ResamplingQueue(card_index, OUTPUT_FREQUENCY, OUTPUT_FREQUENCY, 2));
			dropped_frames = 0;
		} else if (dropped_frames > 0) {
			// Insert silence as needed.
			fprintf(stderr, "Card %d dropped %d frame(s) (before timecode 0x%04x), inserting silence.\n",
				card_index, dropped_frames, timecode);
			vector<float> silence(silence_samples * 2, 0.0f);
			for (int i = 0; i < dropped_frames; ++i) {
				card->resampling_queue->add_input_samples(local_pts / double(TIMEBASE), silence.data(), silence_samples);
				// Note that if the format changed in the meantime, we have
				// no way of detecting that; we just have to assume the frame length
				// is always the same.
				local_pts += frame_length;
			}
		}
		if (num_samples == 0) {
			audio.resize(silence_samples * 2);
			num_samples = silence_samples;
		}
		card->resampling_queue->add_input_samples(local_pts / double(TIMEBASE), audio.data(), num_samples);
		card->next_local_pts = local_pts + frame_length;
	}

	card->last_timecode = timecode;

	// Done with the audio, so release it.
	if (audio_frame.owner) {
		audio_frame.owner->release_frame(audio_frame);
	}

	size_t expected_length = video_format.width * (video_format.height + video_format.extra_lines_top + video_format.extra_lines_bottom) * 2;
	if (video_frame.len - video_offset == 0 ||
	    video_frame.len - video_offset != expected_length) {
		if (video_frame.len != 0) {
			printf("Card %d: Dropping video frame with wrong length (%ld; expected %ld)\n",
				card_index, video_frame.len - video_offset, expected_length);
		}
		if (video_frame.owner) {
			video_frame.owner->release_frame(video_frame);
		}

		// Still send on the information that we _had_ a frame, even though it's corrupted,
		// so that pts can go up accordingly.
		{
			unique_lock<mutex> lock(bmusb_mutex);
			CaptureCard::NewFrame new_frame;
			new_frame.frame = RefCountedFrame(FrameAllocator::Frame());
			new_frame.length = frame_length;
			new_frame.interlaced = false;
			new_frame.dropped_frames = dropped_frames;
			card->new_frames.push(move(new_frame));
			card->new_frames_changed.notify_all();
		}
		return;
	}

	PBOFrameAllocator::Userdata *userdata = (PBOFrameAllocator::Userdata *)video_frame.userdata;

	unsigned num_fields = video_format.interlaced ? 2 : 1;
	steady_clock::time_point frame_upload_start;
	if (video_format.interlaced) {
		// Send the two fields along as separate frames; the other side will need to add
		// a deinterlacer to actually get this right.
		assert(video_format.height % 2 == 0);
		video_format.height /= 2;
		assert(frame_length % 2 == 0);
		frame_length /= 2;
		num_fields = 2;
		frame_upload_start = steady_clock::now();
	}
	userdata->last_interlaced = video_format.interlaced;
	userdata->last_has_signal = video_format.has_signal;
	userdata->last_is_connected = video_format.is_connected;
	userdata->last_frame_rate_nom = video_format.frame_rate_nom;
	userdata->last_frame_rate_den = video_format.frame_rate_den;
	RefCountedFrame frame(video_frame);

	// Upload the textures.
	size_t cbcr_width = video_format.width / 2;
	size_t cbcr_offset = video_offset / 2;
	size_t y_offset = video_frame.size / 2 + video_offset / 2;

	for (unsigned field = 0; field < num_fields; ++field) {
		// Put the actual texture upload in a lambda that is executed in the main thread.
		// It is entirely possible to do this in the same thread (and it might even be
		// faster, depending on the GPU and driver), but it appears to be trickling
		// driver bugs very easily.
		//
		// Note that this means we must hold on to the actual frame data in <userdata>
		// until the upload command is run, but we hold on to <frame> much longer than that
		// (in fact, all the way until we no longer use the texture in rendering).
		auto upload_func = [field, video_format, y_offset, cbcr_offset, cbcr_width, userdata]() {
			unsigned field_start_line = (field == 1) ? video_format.second_field_start : video_format.extra_lines_top + field * (video_format.height + 22);

			if (userdata->tex_y[field] == 0 ||
			    userdata->tex_cbcr[field] == 0 ||
			    video_format.width != userdata->last_width[field] ||
			    video_format.height != userdata->last_height[field]) {
				// We changed resolution since last use of this texture, so we need to create
				// a new object. Note that this each card has its own PBOFrameAllocator,
				// we don't need to worry about these flip-flopping between resolutions.
				glBindTexture(GL_TEXTURE_2D, userdata->tex_cbcr[field]);
				check_error();
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, cbcr_width, video_format.height, 0, GL_RG, GL_UNSIGNED_BYTE, nullptr);
				check_error();
				glBindTexture(GL_TEXTURE_2D, userdata->tex_y[field]);
				check_error();
				glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, video_format.width, video_format.height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
				check_error();
				userdata->last_width[field] = video_format.width;
				userdata->last_height[field] = video_format.height;
			}

			GLuint pbo = userdata->pbo;
			check_error();
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
			check_error();

			size_t field_y_start = y_offset + video_format.width * field_start_line;
			size_t field_cbcr_start = cbcr_offset + cbcr_width * field_start_line * sizeof(uint16_t);

			if (global_flags.flush_pbos) {
				glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER, field_y_start, video_format.width * video_format.height);
				check_error();
				glFlushMappedBufferRange(GL_PIXEL_UNPACK_BUFFER, field_cbcr_start, cbcr_width * video_format.height * sizeof(uint16_t));
				check_error();
			}

			glBindTexture(GL_TEXTURE_2D, userdata->tex_cbcr[field]);
			check_error();
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, cbcr_width, video_format.height, GL_RG, GL_UNSIGNED_BYTE, BUFFER_OFFSET(field_cbcr_start));
			check_error();
			glBindTexture(GL_TEXTURE_2D, userdata->tex_y[field]);
			check_error();
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, video_format.width, video_format.height, GL_RED, GL_UNSIGNED_BYTE, BUFFER_OFFSET(field_y_start));
			check_error();
			glBindTexture(GL_TEXTURE_2D, 0);
			check_error();
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
			check_error();
		};

		if (field == 1) {
			// Don't upload the second field as fast as we can; wait until
			// the field time has approximately passed. (Otherwise, we could
			// get timing jitter against the other sources, and possibly also
			// against the video display, although the latter is not as critical.)
			// This requires our system clock to be reasonably close to the
			// video clock, but that's not an unreasonable assumption.
			steady_clock::time_point second_field_start = frame_upload_start +
				nanoseconds(frame_length * 1000000000 / TIMEBASE);
			this_thread::sleep_until(second_field_start);
		}

		{
			unique_lock<mutex> lock(bmusb_mutex);
			CaptureCard::NewFrame new_frame;
			new_frame.frame = frame;
			new_frame.length = frame_length;
			new_frame.field = field;
			new_frame.interlaced = video_format.interlaced;
			new_frame.upload_func = upload_func;
			new_frame.dropped_frames = dropped_frames;
			card->new_frames.push(move(new_frame));
			card->new_frames_changed.notify_all();
		}
	}
}

void Mixer::bm_hotplug_add(libusb_device *dev)
{
	lock_guard<mutex> lock(hotplug_mutex);
	hotplugged_cards.push_back(dev);
}

void Mixer::bm_hotplug_remove(unsigned card_index)
{
	cards[card_index].new_frames_changed.notify_all();
}

void Mixer::thread_func()
{
	eglBindAPI(EGL_OPENGL_API);
	QOpenGLContext *context = create_context(mixer_surface);
	if (!make_current(context, mixer_surface)) {
		printf("oops\n");
		exit(1);
	}

	steady_clock::time_point start, now;
	start = steady_clock::now();

	int frame = 0;
	int stats_dropped_frames = 0;

	while (!should_quit) {
		CaptureCard::NewFrame new_frames[MAX_CARDS];
		bool has_new_frame[MAX_CARDS] = { false };
		int num_samples[MAX_CARDS] = { 0 };

		unsigned master_card_index = theme->map_signal(master_clock_channel);
		assert(master_card_index < num_cards);

		get_one_frame_from_each_card(master_card_index, new_frames, has_new_frame, num_samples);
		schedule_audio_resampling_tasks(new_frames[master_card_index].dropped_frames, num_samples[master_card_index], new_frames[master_card_index].length);
		stats_dropped_frames += new_frames[master_card_index].dropped_frames;
		send_audio_level_callback();

		handle_hotplugged_cards();

		for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
			if (card_index == master_card_index || !has_new_frame[card_index]) {
				continue;
			}
			if (new_frames[card_index].frame->len == 0) {
				++new_frames[card_index].dropped_frames;
			}
			if (new_frames[card_index].dropped_frames > 0) {
				printf("Card %u dropped %d frames before this\n",
					card_index, int(new_frames[card_index].dropped_frames));
			}
		}

		// If the first card is reporting a corrupted or otherwise dropped frame,
		// just increase the pts (skipping over this frame) and don't try to compute anything new.
		if (new_frames[master_card_index].frame->len == 0) {
			++stats_dropped_frames;
			pts_int += new_frames[master_card_index].length;
			continue;
		}

		for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
			if (!has_new_frame[card_index] || new_frames[card_index].frame->len == 0)
				continue;

			CaptureCard::NewFrame *new_frame = &new_frames[card_index];
			assert(new_frame->frame != nullptr);
			insert_new_frame(new_frame->frame, new_frame->field, new_frame->interlaced, card_index, &input_state);
			check_error();

			// The new texture might need uploading before use.
			if (new_frame->upload_func) {
				new_frame->upload_func();
				new_frame->upload_func = nullptr;
			}
		}

		int64_t frame_duration = new_frames[master_card_index].length;
		render_one_frame(frame_duration);
		++frame;
		pts_int += frame_duration;

		now = steady_clock::now();
		double elapsed = duration<double>(now - start).count();
		if (frame % 100 == 0) {
			printf("%d frames (%d dropped) in %.3f seconds = %.1f fps (%.1f ms/frame)",
				frame, stats_dropped_frames, elapsed, frame / elapsed,
				1e3 * elapsed / frame);
		//	chain->print_phase_timing();

			// Check our memory usage, to see if we are close to our mlockall()
			// limit (if at all set).
			rusage used;
			if (getrusage(RUSAGE_SELF, &used) == -1) {
				perror("getrusage(RUSAGE_SELF)");
				assert(false);
			}

			if (uses_mlock) {
				rlimit limit;
				if (getrlimit(RLIMIT_MEMLOCK, &limit) == -1) {
					perror("getrlimit(RLIMIT_MEMLOCK)");
					assert(false);
				}

				printf(", using %ld / %ld MB lockable memory (%.1f%%)",
					long(used.ru_maxrss / 1024),
					long(limit.rlim_cur / 1048576),
					float(100.0 * (used.ru_maxrss * 1024.0) / limit.rlim_cur));
			} else {
				printf(", using %ld MB memory (not locked)",
					long(used.ru_maxrss / 1024));
			}

			printf("\n");
		}


		if (should_cut.exchange(false)) {  // Test and clear.
			video_encoder->do_cut(frame);
		}

#if 0
		// Reset every 100 frames, so that local variations in frame times
		// (especially for the first few frames, when the shaders are
		// compiled etc.) don't make it hard to measure for the entire
		// remaining duration of the program.
		if (frame == 10000) {
			frame = 0;
			start = now;
		}
#endif
		check_error();
	}

	resource_pool->clean_context();
}

void Mixer::get_one_frame_from_each_card(unsigned master_card_index, CaptureCard::NewFrame new_frames[MAX_CARDS], bool has_new_frame[MAX_CARDS], int num_samples[MAX_CARDS])
{
start:
	// The first card is the master timer, so wait for it to have a new frame.
	// TODO: Add a timeout.
	unique_lock<mutex> lock(bmusb_mutex);
	cards[master_card_index].new_frames_changed.wait(lock, [this, master_card_index]{ return !cards[master_card_index].new_frames.empty() || cards[master_card_index].capture->get_disconnected(); });

	if (cards[master_card_index].new_frames.empty()) {
		// We were woken up, but not due to a new frame. Deal with it
		// and then restart.
		assert(cards[master_card_index].capture->get_disconnected());
		handle_hotplugged_cards();
		goto start;
	}

	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		CaptureCard *card = &cards[card_index];
		if (card->new_frames.empty()) {
			assert(card_index != master_card_index);
			card->queue_length_policy.update_policy(-1);
			continue;
		}
		new_frames[card_index] = move(card->new_frames.front());
		has_new_frame[card_index] = true;
		card->new_frames.pop();
		card->new_frames_changed.notify_all();

		int num_samples_times_timebase = OUTPUT_FREQUENCY * new_frames[card_index].length + card->fractional_samples;
		num_samples[card_index] = num_samples_times_timebase / TIMEBASE;
		card->fractional_samples = num_samples_times_timebase % TIMEBASE;
		assert(num_samples[card_index] >= 0);

		if (card_index == master_card_index) {
			// We don't use the queue length policy for the master card,
			// but we will if it stops being the master. Thus, clear out
			// the policy in case we switch in the future.
			card->queue_length_policy.reset(card_index);
		} else {
			// If we have excess frames compared to the policy for this card,
			// drop frames from the head.
			card->queue_length_policy.update_policy(card->new_frames.size());
			while (card->new_frames.size() > card->queue_length_policy.get_safe_queue_length()) {
				card->new_frames.pop();
			}
		}
	}
}

void Mixer::handle_hotplugged_cards()
{
	// Check for cards that have been disconnected since last frame.
	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		CaptureCard *card = &cards[card_index];
		if (card->capture->get_disconnected()) {
			fprintf(stderr, "Card %u went away, replacing with a fake card.\n", card_index);
			FakeCapture *capture = new FakeCapture(WIDTH, HEIGHT, FAKE_FPS, OUTPUT_FREQUENCY, card_index, global_flags.fake_cards_audio);
			configure_card(card_index, capture, /*is_fake_capture=*/true);
			card->queue_length_policy.reset(card_index);
			card->capture->start_bm_capture();
		}
	}

	// Check for cards that have been connected since last frame.
	vector<libusb_device *> hotplugged_cards_copy;
	{
		lock_guard<mutex> lock(hotplug_mutex);
		swap(hotplugged_cards, hotplugged_cards_copy);
	}
	for (libusb_device *new_dev : hotplugged_cards_copy) {
		// Look for a fake capture card where we can stick this in.
		int free_card_index = -1;
		for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
			if (cards[card_index].is_fake_capture) {
				free_card_index = int(card_index);
				break;
			}
		}

		if (free_card_index == -1) {
			fprintf(stderr, "New card plugged in, but no free slots -- ignoring.\n");
			libusb_unref_device(new_dev);
		} else {
			// BMUSBCapture takes ownership.
			fprintf(stderr, "New card plugged in, choosing slot %d.\n", free_card_index);
			CaptureCard *card = &cards[free_card_index];
			BMUSBCapture *capture = new BMUSBCapture(free_card_index, new_dev);
			configure_card(free_card_index, capture, /*is_fake_capture=*/false);
			card->queue_length_policy.reset(free_card_index);
			capture->set_card_disconnected_callback(bind(&Mixer::bm_hotplug_remove, this, free_card_index));
			capture->start_bm_capture();
		}
	}
}


void Mixer::schedule_audio_resampling_tasks(unsigned dropped_frames, int num_samples_per_frame, int length_per_frame)
{
	// Resample the audio as needed, including from previously dropped frames.
	assert(num_cards > 0);
	for (unsigned frame_num = 0; frame_num < dropped_frames + 1; ++frame_num) {
		const bool dropped_frame = (frame_num != dropped_frames);
		{
			// Signal to the audio thread to process this frame.
			// Note that if the frame is a dropped frame, we signal that
			// we don't want to use this frame as base for adjusting
			// the resampler rate. The reason for this is that the timing
			// of these frames is often way too late; they typically don't
			// “arrive” before we synthesize them. Thus, we could end up
			// in a situation where we have inserted e.g. five audio frames
			// into the queue before we then start pulling five of them
			// back out. This makes ResamplingQueue overestimate the delay,
			// causing undue resampler changes. (We _do_ use the last,
			// non-dropped frame; perhaps we should just discard that as well,
			// since dropped frames are expected to be rare, and it might be
			// better to just wait until we have a slightly more normal situation).
			unique_lock<mutex> lock(audio_mutex);
			bool adjust_rate = !dropped_frame;
			audio_task_queue.push(AudioTask{pts_int, num_samples_per_frame, adjust_rate});
			audio_task_queue_changed.notify_one();
		}
		if (dropped_frame) {
			// For dropped frames, increase the pts. Note that if the format changed
			// in the meantime, we have no way of detecting that; we just have to
			// assume the frame length is always the same.
			pts_int += length_per_frame;
		}
	}
}

void Mixer::render_one_frame(int64_t duration)
{
	// Get the main chain from the theme, and set its state immediately.
	Theme::Chain theme_main_chain = theme->get_chain(0, pts(), WIDTH, HEIGHT, input_state);
	EffectChain *chain = theme_main_chain.chain;
	theme_main_chain.setup_chain();
	//theme_main_chain.chain->enable_phase_timing(true);

	GLuint y_tex, cbcr_tex;
	bool got_frame = video_encoder->begin_frame(&y_tex, &cbcr_tex);
	assert(got_frame);

	// Render main chain.
	GLuint cbcr_full_tex = resource_pool->create_2d_texture(GL_RG8, WIDTH, HEIGHT);
	GLuint rgba_tex = resource_pool->create_2d_texture(GL_RGB565, WIDTH, HEIGHT);  // Saves texture bandwidth, although dithering gets messed up.
	GLuint fbo = resource_pool->create_fbo(y_tex, cbcr_full_tex, rgba_tex);
	check_error();
	chain->render_to_fbo(fbo, WIDTH, HEIGHT);
	resource_pool->release_fbo(fbo);

	subsample_chroma(cbcr_full_tex, cbcr_tex);
	resource_pool->release_2d_texture(cbcr_full_tex);

	// Set the right state for rgba_tex.
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, rgba_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	const int64_t av_delay = TIMEBASE / 10;  // Corresponds to the fixed delay in resampling_queue.h. TODO: Make less hard-coded.
	RefCountedGLsync fence = video_encoder->end_frame(pts_int + av_delay, duration, theme_main_chain.input_frames);

	// The live frame just shows the RGBA texture we just rendered.
	// It owns rgba_tex now.
	DisplayFrame live_frame;
	live_frame.chain = display_chain.get();
	live_frame.setup_chain = [this, rgba_tex]{
		display_input->set_texture_num(rgba_tex);
	};
	live_frame.ready_fence = fence;
	live_frame.input_frames = {};
	live_frame.temp_textures = { rgba_tex };
	output_channel[OUTPUT_LIVE].output_frame(live_frame);

	// Set up preview and any additional channels.
	for (int i = 1; i < theme->get_num_channels() + 2; ++i) {
		DisplayFrame display_frame;
		Theme::Chain chain = theme->get_chain(i, pts(), WIDTH, HEIGHT, input_state);  // FIXME: dimensions
		display_frame.chain = chain.chain;
		display_frame.setup_chain = chain.setup_chain;
		display_frame.ready_fence = fence;
		display_frame.input_frames = chain.input_frames;
		display_frame.temp_textures = {};
		output_channel[i].output_frame(display_frame);
	}
}

void Mixer::send_audio_level_callback()
{
	if (audio_level_callback == nullptr) {
		return;
	}

	unique_lock<mutex> lock(compressor_mutex);
	double loudness_s = r128.loudness_S();
	double loudness_i = r128.integrated();
	double loudness_range_low = r128.range_min();
	double loudness_range_high = r128.range_max();

	audio_level_callback(loudness_s, 20.0 * log10(peak),
		loudness_i, loudness_range_low, loudness_range_high,
		gain_staging_db, 20.0 * log10(final_makeup_gain),
		correlation.get_correlation());
}

void Mixer::audio_thread_func()
{
	while (!should_quit) {
		AudioTask task;

		{
			unique_lock<mutex> lock(audio_mutex);
			audio_task_queue_changed.wait(lock, [this]{ return should_quit || !audio_task_queue.empty(); });
			if (should_quit) {
				return;
			}
			task = audio_task_queue.front();
			audio_task_queue.pop();
		}

		process_audio_one_frame(task.pts_int, task.num_samples, task.adjust_rate);
	}
}

void Mixer::process_audio_one_frame(int64_t frame_pts_int, int num_samples, bool adjust_rate)
{
	vector<float> samples_card;
	vector<float> samples_out;

	// TODO: Allow mixing audio from several sources.
	unsigned selected_audio_card = theme->map_signal(audio_source_channel);
	assert(selected_audio_card < num_cards);

	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		samples_card.resize(num_samples * 2);
		{
			unique_lock<mutex> lock(cards[card_index].audio_mutex);
			ResamplingQueue::RateAdjustmentPolicy rate_adjustment_policy =
				adjust_rate ? ResamplingQueue::ADJUST_RATE : ResamplingQueue::DO_NOT_ADJUST_RATE;
			cards[card_index].resampling_queue->get_output_samples(
				double(frame_pts_int) / TIMEBASE,
				&samples_card[0],
				num_samples,
				rate_adjustment_policy);
		}
		if (card_index == selected_audio_card) {
			samples_out = move(samples_card);
		}
	}

	// Cut away everything under 120 Hz (or whatever the cutoff is);
	// we don't need it for voice, and it will reduce headroom
	// and confuse the compressor. (In particular, any hums at 50 or 60 Hz
	// should be dampened.)
	if (locut_enabled) {
		locut.render(samples_out.data(), samples_out.size() / 2, locut_cutoff_hz * 2.0 * M_PI / OUTPUT_FREQUENCY, 0.5f);
	}

	// Apply a level compressor to get the general level right.
	// Basically, if it's over about -40 dBFS, we squeeze it down to that level
	// (or more precisely, near it, since we don't use infinite ratio),
	// then apply a makeup gain to get it to -14 dBFS. -14 dBFS is, of course,
	// entirely arbitrary, but from practical tests with speech, it seems to
	// put ut around -23 LUFS, so it's a reasonable starting point for later use.
	{
		unique_lock<mutex> lock(compressor_mutex);
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
	{
		unique_lock<mutex> lock(compressor_mutex);
		double loudness_lu = r128.loudness_M() - ref_level_lufs;
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

		double m = final_makeup_gain;
		for (size_t i = 0; i < samples_out.size(); i += 2) {
			samples_out[i + 0] *= m;
			samples_out[i + 1] *= m;
			m += (target_loudness_factor - m) * alpha;
		}
		final_makeup_gain = m;
	}

	// Upsample 4x to find interpolated peak.
	peak_resampler.inp_data = samples_out.data();
	peak_resampler.inp_count = samples_out.size() / 2;

	vector<float> interpolated_samples_out;
	interpolated_samples_out.resize(samples_out.size());
	while (peak_resampler.inp_count > 0) {  // About four iterations.
		peak_resampler.out_data = &interpolated_samples_out[0];
		peak_resampler.out_count = interpolated_samples_out.size() / 2;
		peak_resampler.process();
		size_t out_stereo_samples = interpolated_samples_out.size() / 2 - peak_resampler.out_count;
		peak = max<float>(peak, find_peak(interpolated_samples_out.data(), out_stereo_samples * 2));
		peak_resampler.out_data = nullptr;
	}

	// Find R128 levels and L/R correlation.
	vector<float> left, right;
	deinterleave_samples(samples_out, &left, &right);
	float *ptrs[] = { left.data(), right.data() };
	{
		unique_lock<mutex> lock(compressor_mutex);
		r128.process(left.size(), ptrs);
		correlation.process_samples(samples_out);
	}

	// Send the samples to the sound card.
	if (alsa) {
		alsa->write(samples_out);
	}

	// And finally add them to the output.
	video_encoder->add_audio(frame_pts_int, move(samples_out));
}

void Mixer::subsample_chroma(GLuint src_tex, GLuint dst_tex)
{
	GLuint vao;
	glGenVertexArrays(1, &vao);
	check_error();

	glBindVertexArray(vao);
	check_error();

	// Extract Cb/Cr.
	GLuint fbo = resource_pool->create_fbo(dst_tex);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, WIDTH/2, HEIGHT/2);
	check_error();

	glUseProgram(cbcr_program_num);
	check_error();

	glActiveTexture(GL_TEXTURE0);
	check_error();
	glBindTexture(GL_TEXTURE_2D, src_tex);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();

	float chroma_offset_0[] = { -0.5f / WIDTH, 0.0f };
	set_uniform_vec2(cbcr_program_num, "foo", "chroma_offset_0", chroma_offset_0);

	glBindBuffer(GL_ARRAY_BUFFER, cbcr_vbo);
	check_error();

	for (GLint attr_index : { cbcr_position_attribute_index, cbcr_texcoord_attribute_index }) {
		glEnableVertexAttribArray(attr_index);
		check_error();
		glVertexAttribPointer(attr_index, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));
		check_error();
	}

	glDrawArrays(GL_TRIANGLES, 0, 3);
	check_error();

	for (GLint attr_index : { cbcr_position_attribute_index, cbcr_texcoord_attribute_index }) {
		glDisableVertexAttribArray(attr_index);
		check_error();
	}

	glUseProgram(0);
	check_error();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	check_error();

	resource_pool->release_fbo(fbo);
	glDeleteVertexArrays(1, &vao);
}

void Mixer::release_display_frame(DisplayFrame *frame)
{
	for (GLuint texnum : frame->temp_textures) {
		resource_pool->release_2d_texture(texnum);
	}
	frame->temp_textures.clear();
	frame->ready_fence.reset();
	frame->input_frames.clear();
}

void Mixer::start()
{
	mixer_thread = thread(&Mixer::thread_func, this);
	audio_thread = thread(&Mixer::audio_thread_func, this);
}

void Mixer::quit()
{
	should_quit = true;
	audio_task_queue_changed.notify_one();
	mixer_thread.join();
	audio_thread.join();
}

void Mixer::transition_clicked(int transition_num)
{
	theme->transition_clicked(transition_num, pts());
}

void Mixer::channel_clicked(int preview_num)
{
	theme->channel_clicked(preview_num);
}

void Mixer::reset_meters()
{
	peak_resampler.reset();
	peak = 0.0f;
	r128.reset();
	r128.integr_start();
	correlation.reset();
}

void Mixer::start_mode_scanning(unsigned card_index)
{
	assert(card_index < num_cards);
	if (is_mode_scanning[card_index]) {
		return;
	}
	is_mode_scanning[card_index] = true;
	mode_scanlist[card_index].clear();
	for (const auto &mode : cards[card_index].capture->get_available_video_modes()) {
		mode_scanlist[card_index].push_back(mode.first);
	}
	assert(!mode_scanlist[card_index].empty());
	mode_scanlist_index[card_index] = 0;
	cards[card_index].capture->set_video_mode(mode_scanlist[card_index][0]);
	last_mode_scan_change[card_index] = steady_clock::now();
}

Mixer::OutputChannel::~OutputChannel()
{
	if (has_current_frame) {
		parent->release_display_frame(&current_frame);
	}
	if (has_ready_frame) {
		parent->release_display_frame(&ready_frame);
	}
}

void Mixer::OutputChannel::output_frame(DisplayFrame frame)
{
	// Store this frame for display. Remove the ready frame if any
	// (it was seemingly never used).
	{
		unique_lock<mutex> lock(frame_mutex);
		if (has_ready_frame) {
			parent->release_display_frame(&ready_frame);
		}
		ready_frame = frame;
		has_ready_frame = true;
	}

	if (new_frame_ready_callback) {
		new_frame_ready_callback();
	}

	// Reduce the number of callbacks by filtering duplicates. The reason
	// why we bother doing this is that Qt seemingly can get into a state
	// where its builds up an essentially unbounded queue of signals,
	// consuming more and more memory, and there's no good way of collapsing
	// user-defined signals or limiting the length of the queue.
	if (transition_names_updated_callback) {
		vector<string> transition_names = global_mixer->get_transition_names();
		bool changed = false;
		if (transition_names.size() != last_transition_names.size()) {
			changed = true;
		} else {
			for (unsigned i = 0; i < transition_names.size(); ++i) {
				if (transition_names[i] != last_transition_names[i]) {
					changed = true;
					break;
				}
			}
		}
		if (changed) {
			transition_names_updated_callback(transition_names);
			last_transition_names = transition_names;
		}
	}
	if (name_updated_callback) {
		string name = global_mixer->get_channel_name(channel);
		if (name != last_name) {
			name_updated_callback(name);
			last_name = name;
		}
	}
	if (color_updated_callback) {
		string color = global_mixer->get_channel_color(channel);
		if (color != last_color) {
			color_updated_callback(color);
			last_color = color;
		}
	}
}

bool Mixer::OutputChannel::get_display_frame(DisplayFrame *frame)
{
	unique_lock<mutex> lock(frame_mutex);
	if (!has_current_frame && !has_ready_frame) {
		return false;
	}

	if (has_current_frame && has_ready_frame) {
		// We have a new ready frame. Toss the current one.
		parent->release_display_frame(&current_frame);
		has_current_frame = false;
	}
	if (has_ready_frame) {
		assert(!has_current_frame);
		current_frame = ready_frame;
		ready_frame.ready_fence.reset();  // Drop the refcount.
		ready_frame.input_frames.clear();  // Drop the refcounts.
		has_current_frame = true;
		has_ready_frame = false;
	}

	*frame = current_frame;
	return true;
}

void Mixer::OutputChannel::set_frame_ready_callback(Mixer::new_frame_ready_callback_t callback)
{
	new_frame_ready_callback = callback;
}

void Mixer::OutputChannel::set_transition_names_updated_callback(Mixer::transition_names_updated_callback_t callback)
{
	transition_names_updated_callback = callback;
}

void Mixer::OutputChannel::set_name_updated_callback(Mixer::name_updated_callback_t callback)
{
	name_updated_callback = callback;
}

void Mixer::OutputChannel::set_color_updated_callback(Mixer::color_updated_callback_t callback)
{
	color_updated_callback = callback;
}

mutex RefCountedGLsync::fence_lock;
