#include <DeckLinkAPI.h>
#include <DeckLinkAPIModes.h>

#include "decklink_util.h"

using namespace bmusb;
using namespace std;

map<uint32_t, VideoMode> summarize_video_modes(IDeckLinkDisplayModeIterator *mode_it, unsigned card_index)
{
	map<uint32_t, VideoMode> video_modes;

	for (IDeckLinkDisplayMode *mode_ptr; mode_it->Next(&mode_ptr) == S_OK; mode_ptr->Release()) {
		VideoMode mode;

		const char *mode_name;
		if (mode_ptr->GetName(&mode_name) != S_OK) {
			mode.name = "Unknown mode";
		} else {
			mode.name = mode_name;
		}

		mode.autodetect = false;

		mode.width = mode_ptr->GetWidth();
		mode.height = mode_ptr->GetHeight();

		BMDTimeScale frame_rate_num;
		BMDTimeValue frame_rate_den;
		if (mode_ptr->GetFrameRate(&frame_rate_den, &frame_rate_num) != S_OK) {
			fprintf(stderr, "Could not get frame rate for mode '%s' on card %d\n", mode.name.c_str(), card_index);
			exit(1);
		}
		mode.frame_rate_num = frame_rate_num;
		mode.frame_rate_den = frame_rate_den;

		// TODO: Respect the TFF/BFF flag.
		mode.interlaced = (mode_ptr->GetFieldDominance() == bmdLowerFieldFirst || mode_ptr->GetFieldDominance() == bmdUpperFieldFirst);

		uint32_t id = mode_ptr->GetDisplayMode();
		video_modes.insert(make_pair(id, mode));
	}

	return video_modes;
}
