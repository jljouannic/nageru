#ifndef _THEME_H
#define _THEME_H 1

#include <lua.hpp>
#include <movit/ycbcr_input.h>
#include <stdbool.h>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "bmusb/bmusb.h"
#include "ref_counted_frame.h"

struct InputState;

namespace movit {
class ResourcePool;
class Effect;
class EffectChain;
struct ImageFormat;
struct YCbCrFormat;
}  // namespace movit

class NonBouncingYCbCrInput : public movit::YCbCrInput {
public:
	NonBouncingYCbCrInput(const movit::ImageFormat &image_format,
	                      const movit::YCbCrFormat &ycbcr_format,
	                      unsigned width, unsigned height,
	                      movit::YCbCrInputSplitting ycbcr_input_splitting = movit::YCBCR_INPUT_PLANAR)
	    : movit::YCbCrInput(image_format, ycbcr_format, width, height, ycbcr_input_splitting) {}

	bool override_disable_bounce() const override { return true; }
};

class Theme {
public:
	Theme(const std::string &filename, const std::vector<std::string> &search_dirs, movit::ResourcePool *resource_pool, unsigned num_cards);
	~Theme();

	struct Chain {
		movit::EffectChain *chain;
		std::function<void()> setup_chain;

		// May have duplicates.
		std::vector<RefCountedFrame> input_frames;
	};

	Chain get_chain(unsigned num, float t, unsigned width, unsigned height, InputState input_state);

	int get_num_channels() const { return num_channels; }
	int map_signal(int signal_num);
	void set_signal_mapping(int signal_num, int card_num);
	std::string get_channel_name(unsigned channel);
	int get_channel_signal(unsigned channel);
	bool get_supports_set_wb(unsigned channel);
	void set_wb(unsigned channel, double r, double g, double b);
	std::string get_channel_color(unsigned channel);

	std::vector<std::string> get_transition_names(float t);

	void transition_clicked(int transition_num, float t);
	void channel_clicked(int preview_num);

	movit::ResourcePool *get_resource_pool() const { return resource_pool; }

private:
	void register_class(const char *class_name, const luaL_Reg *funcs);

	std::mutex m;
	lua_State *L;  // Protected by <m>.
	const InputState *input_state;  // Protected by <m>. Only set temporarily, during chain setup.
	movit::ResourcePool *resource_pool;
	int num_channels;
	unsigned num_cards;

	std::mutex map_m;
	std::map<int, int> signal_to_card_mapping;  // Protected by <map_m>.

	friend class LiveInputWrapper;
};

// LiveInputWrapper is a facade on top of an YCbCrInput, exposed to
// the Lua code. It contains a function (connect_signal()) intended
// to be called during chain setup, that picks out the current frame
// (in the form of a set of textures) from the input state given by
// the mixer, and communicates that state over to the actual YCbCrInput.
class LiveInputWrapper {
public:
	LiveInputWrapper(Theme *theme, movit::EffectChain *chain, bmusb::PixelFormat pixel_format, bool override_bounce, bool deinterlace);

	void connect_signal(int signal_num);
	movit::Effect *get_effect() const
	{
		if (deinterlace) {
			return deinterlace_effect;
		} else {
			return inputs[0];
		}
	}

private:
	Theme *theme;  // Not owned by us.
	bmusb::PixelFormat pixel_format;
	std::vector<movit::YCbCrInput *> inputs;  // Multiple ones if deinterlacing. Owned by the chain.
	movit::Effect *deinterlace_effect = nullptr;  // Owned by the chain.
	bool deinterlace;
};

#endif  // !defined(_THEME_H)
