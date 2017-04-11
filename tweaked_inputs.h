#ifndef _TWEAKED_INPUTS_H
#define _TWEAKED_INPUTS_H 1

// Some tweaked variations of Movit inputs.

#include <movit/ycbcr_input.h>

namespace movit {
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

#endif  // !defined(_TWEAKED_INPUTS_H)
