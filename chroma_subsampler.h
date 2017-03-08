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
	//
	// You can get two equal copies if you'd like; just set dst2_tex to a texture
	// number and it will receive an exact copy of what goes into dst_tex.
	void subsample_chroma(GLuint cbcr_tex, unsigned width, unsigned height, GLuint dst_tex, GLuint dst2_tex = 0);

	// Subsamples and interleaves luma and chroma to give 4:2:2 packed Y'CbCr (UYVY).
	// Chroma positioning is left (H.264 convention).
	// width and height are the dimensions (in pixels) of the input textures.
	void create_uyvy(GLuint y_tex, GLuint cbcr_tex, unsigned width, unsigned height, GLuint dst_tex);

private:
	movit::ResourcePool *resource_pool;

	GLuint vbo;  // Holds position and texcoord data.

	GLuint cbcr_program_num;  // Owned by <resource_pool>.
	GLuint cbcr_texture_sampler_uniform;
	GLuint cbcr_position_attribute_index, cbcr_texcoord_attribute_index;

	GLuint uyvy_program_num;  // Owned by <resource_pool>.
	GLuint uyvy_y_texture_sampler_uniform, uyvy_cbcr_texture_sampler_uniform;
	GLuint uyvy_position_attribute_index, uyvy_texcoord_attribute_index;
};

#endif  // !defined(_CHROMA_SUBSAMPLER_H)
