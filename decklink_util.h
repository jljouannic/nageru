#ifndef _DECKLINK_UTIL
#define _DECKLINK_UTIL 1

#include <stdint.h>

#include <map>

#include "bmusb/bmusb.h"

class IDeckLinkDisplayModeIterator;

std::map<uint32_t, bmusb::VideoMode> summarize_video_modes(IDeckLinkDisplayModeIterator *mode_it, unsigned card_index);

#endif  // !defined(_DECKLINK_UTIL)
