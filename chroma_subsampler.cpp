#include "chroma_subsampler.h"

#include <vector>

#include <movit/effect_util.h>
#include <movit/resource_pool.h>
#include <movit/util.h>

using namespace movit;
using namespace std;

ChromaSubsampler::ChromaSubsampler(ResourcePool *resource_pool)
	: resource_pool(resource_pool)
{
	// Set up stuff for NV12 conversion.
	//
	// Note: Due to the horizontally co-sited chroma/luma samples in H.264
	// (chrome position is left for horizontal and center for vertical),
	// we need to be a bit careful in our subsampling. A diagram will make
	// this clearer, showing some luma and chroma samples:
	//
	//     a   b   c   d
	//   +---+---+---+---+
	//   |   |   |   |   |
	//   | Y | Y | Y | Y |
	//   |   |   |   |   |
	//   +---+---+---+---+
	//
	// +-------+-------+
	// |       |       |
	// |   C   |   C   |
	// |       |       |
	// +-------+-------+
	//
	// Clearly, the rightmost chroma sample here needs to be equivalent to
	// b/4 + c/2 + d/4. (We could also implement more sophisticated filters,
	// of course, but as long as the upsampling is not going to be equally
	// sophisticated, it's probably not worth it.) If we sample once with
	// no mipmapping, we get just c, ie., no actual filtering in the
	// horizontal direction. (For the vertical direction, we can just
	// sample in the middle to get the right filtering.) One could imagine
	// we could use mipmapping (assuming we can create mipmaps cheaply),
	// but then, what we'd get is this:
	//
	//    (a+b)/2 (c+d)/2
	//   +-------+-------+
	//   |       |       |
	//   |   Y   |   Y   |
	//   |       |       |
	//   +-------+-------+
	//
	// +-------+-------+
	// |       |       |
	// |   C   |   C   |
	// |       |       |
	// +-------+-------+
	//
	// which ends up sampling equally from a and b, which clearly isn't right. Instead,
	// we need to do two (non-mipmapped) chroma samples, both hitting exactly in-between
	// source pixels.
	//
	// Sampling in-between b and c gives us the sample (b+c)/2, and similarly for c and d.
	// Taking the average of these gives of (b+c)/4 + (c+d)/4 = b/4 + c/2 + d/4, which is
	// exactly what we want.
	//
	// See also http://www.poynton.com/PDFs/Merging_RGB_and_422.pdf, pages 6–7.

	// Cb/Cr shader.
	string cbcr_vert_shader =
		"#version 130 \n"
		" \n"
		"in vec2 position; \n"
		"in vec2 texcoord; \n"
		"out vec2 tc0, tc1; \n"
		"uniform vec2 foo_chroma_offset_0; \n"
		"uniform vec2 foo_chroma_offset_1; \n"
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
		"    tc1 = flipped_tc + foo_chroma_offset_1; \n"
		"} \n";
	string cbcr_frag_shader =
		"#version 130 \n"
		"in vec2 tc0, tc1; \n"
		"uniform sampler2D cbcr_tex; \n"
		"out vec4 FragColor; \n"
		"void main() { \n"
		"    FragColor = 0.5 * (texture(cbcr_tex, tc0) + texture(cbcr_tex, tc1)); \n"
		"} \n";
	vector<string> frag_shader_outputs;
	cbcr_program_num = resource_pool->compile_glsl_program(cbcr_vert_shader, cbcr_frag_shader, frag_shader_outputs);
	check_error();

	float vertices[] = {
		0.0f, 2.0f,
		0.0f, 0.0f,
		2.0f, 0.0f
	};
	cbcr_vbo = generate_vbo(2, GL_FLOAT, sizeof(vertices), vertices);
	check_error();
	cbcr_texture_sampler_uniform = glGetUniformLocation(cbcr_program_num, "cbcr_tex");
	check_error();
	cbcr_position_attribute_index = glGetAttribLocation(cbcr_program_num, "position");
	check_error();
	cbcr_texcoord_attribute_index = glGetAttribLocation(cbcr_program_num, "texcoord");
	check_error();
}

ChromaSubsampler::~ChromaSubsampler()
{
	resource_pool->release_glsl_program(cbcr_program_num);
	check_error();
	glDeleteBuffers(1, &cbcr_vbo);
	check_error();
}

void ChromaSubsampler::subsample_chroma(GLuint cbcr_tex, unsigned width, unsigned height, GLuint dst_tex)
{
	GLuint vao;
	glGenVertexArrays(1, &vao);
	check_error();

	glBindVertexArray(vao);
	check_error();

	// Extract Cb/Cr.
	GLuint fbo = resource_pool->create_fbo(dst_tex);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, width/2, height/2);
	check_error();

	glUseProgram(cbcr_program_num);
	check_error();

	glActiveTexture(GL_TEXTURE0);
	check_error();
	glBindTexture(GL_TEXTURE_2D, cbcr_tex);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();

	float chroma_offset_0[] = { -1.0f / width, 0.0f };
	float chroma_offset_1[] = { -0.0f / width, 0.0f };
	set_uniform_vec2(cbcr_program_num, "foo", "chroma_offset_0", chroma_offset_0);
	set_uniform_vec2(cbcr_program_num, "foo", "chroma_offset_1", chroma_offset_1);

	glUniform1i(cbcr_texture_sampler_uniform, 0);

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
	check_error();
}
