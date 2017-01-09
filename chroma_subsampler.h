#ifndef _CHROMA_SUBSAMPLER_H
#define _CHROMA_SUBSAMPLER_H 1

#include <epoxy/gl.h>

namespace movit {

class ResourcePool;

}  // namespace movit

class ChromaSubsampler {
public:
	ChromaSubsampler(movit::ResourcePool *resource_pool);
	~ChromaSubsampler();

	// Subsamples chroma (packed Cb and Cr) 2x2 to yield chroma suitable for
	// NV12 (semiplanar 4:2:0). Chroma positioning is left/center (H.264 convention).
	// width and height are the dimensions (in pixels) of the input texture.
	void subsample_chroma(GLuint cbcr_tex, unsigned width, unsigned height, GLuint dst_tex);

private:
	movit::ResourcePool *resource_pool;

	GLuint cbcr_program_num;  // Owned by <resource_pool>.
	GLuint cbcr_texture_sampler_uniform;
	GLuint cbcr_vbo;  // Holds position and texcoord data.
	GLuint cbcr_position_attribute_index, cbcr_texcoord_attribute_index;
};

#endif  // !defined(_CHROMA_SUBSAMPLER_H)
