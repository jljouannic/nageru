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
	vector<string> frag_shader_outputs;

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
		"out vec4 FragColor, FragColor2; \n"
		"void main() { \n"
		"    FragColor = 0.5 * (texture(cbcr_tex, tc0) + texture(cbcr_tex, tc1)); \n"
		"    FragColor2 = FragColor; \n"
		"} \n";
	cbcr_program_num = resource_pool->compile_glsl_program(cbcr_vert_shader, cbcr_frag_shader, frag_shader_outputs);
	check_error();

	cbcr_texture_sampler_uniform = glGetUniformLocation(cbcr_program_num, "cbcr_tex");
	check_error();
	cbcr_position_attribute_index = glGetAttribLocation(cbcr_program_num, "position");
	check_error();
	cbcr_texcoord_attribute_index = glGetAttribLocation(cbcr_program_num, "texcoord");
	check_error();

	// Same, for UYVY conversion.
	string uyvy_vert_shader =
		"#version 130 \n"
		" \n"
		"in vec2 position; \n"
		"in vec2 texcoord; \n"
		"out vec2 y_tc0, y_tc1, cbcr_tc0, cbcr_tc1; \n"
		"uniform vec2 foo_luma_offset_0; \n"
		"uniform vec2 foo_luma_offset_1; \n"
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
		"    y_tc0 = flipped_tc + foo_luma_offset_0; \n"
		"    y_tc1 = flipped_tc + foo_luma_offset_1; \n"
		"    cbcr_tc0 = flipped_tc + foo_chroma_offset_0; \n"
		"    cbcr_tc1 = flipped_tc + foo_chroma_offset_1; \n"
		"} \n";
	string uyvy_frag_shader =
		"#version 130 \n"
		"in vec2 y_tc0, y_tc1, cbcr_tc0, cbcr_tc1; \n"
		"uniform sampler2D y_tex, cbcr_tex; \n"
		"out vec4 FragColor; \n"
		"void main() { \n"
		"    float y0 = texture(y_tex, y_tc0).r; \n"
		"    float y1 = texture(y_tex, y_tc1).r; \n"
		"    vec2 cbcr0 = texture(cbcr_tex, cbcr_tc0).rg; \n"
		"    vec2 cbcr1 = texture(cbcr_tex, cbcr_tc1).rg; \n"
		"    vec2 cbcr = 0.5 * (cbcr0 + cbcr1); \n"
		"    FragColor = vec4(cbcr.g, y0, cbcr.r, y1); \n"
		"} \n";

	uyvy_program_num = resource_pool->compile_glsl_program(uyvy_vert_shader, uyvy_frag_shader, frag_shader_outputs);
	check_error();

	uyvy_y_texture_sampler_uniform = glGetUniformLocation(uyvy_program_num, "y_tex");
	check_error();
	uyvy_cbcr_texture_sampler_uniform = glGetUniformLocation(uyvy_program_num, "cbcr_tex");
	check_error();
	uyvy_position_attribute_index = glGetAttribLocation(uyvy_program_num, "position");
	check_error();
	uyvy_texcoord_attribute_index = glGetAttribLocation(uyvy_program_num, "texcoord");
	check_error();

	// Shared between the two.
	float vertices[] = {
		0.0f, 2.0f,
		0.0f, 0.0f,
		2.0f, 0.0f
	};
	vbo = generate_vbo(2, GL_FLOAT, sizeof(vertices), vertices);
	check_error();
}

ChromaSubsampler::~ChromaSubsampler()
{
	resource_pool->release_glsl_program(cbcr_program_num);
	check_error();
	resource_pool->release_glsl_program(uyvy_program_num);
	check_error();
	glDeleteBuffers(1, &vbo);
	check_error();
}

void ChromaSubsampler::subsample_chroma(GLuint cbcr_tex, unsigned width, unsigned height, GLuint dst_tex, GLuint dst2_tex)
{
	GLuint vao;
	glGenVertexArrays(1, &vao);
	check_error();

	glBindVertexArray(vao);
	check_error();

	// Extract Cb/Cr.
	GLuint fbo;
	if (dst2_tex <= 0) {
		fbo = resource_pool->create_fbo(dst_tex);
	} else {
		fbo = resource_pool->create_fbo(dst_tex, dst2_tex);
	}
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

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
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

void ChromaSubsampler::create_uyvy(GLuint y_tex, GLuint cbcr_tex, unsigned width, unsigned height, GLuint dst_tex)
{
	GLuint vao;
	glGenVertexArrays(1, &vao);
	check_error();

	glBindVertexArray(vao);
	check_error();

	GLuint fbo = resource_pool->create_fbo(dst_tex);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, width/2, height);
	check_error();

	glUseProgram(uyvy_program_num);
	check_error();

	glUniform1i(uyvy_y_texture_sampler_uniform, 0);
	check_error();
	glUniform1i(uyvy_cbcr_texture_sampler_uniform, 1);
	check_error();

	glActiveTexture(GL_TEXTURE0);
	check_error();
	glBindTexture(GL_TEXTURE_2D, y_tex);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();

	glActiveTexture(GL_TEXTURE1);
	check_error();
	glBindTexture(GL_TEXTURE_2D, cbcr_tex);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	check_error();
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	check_error();

	float y_offset_0[] = { -0.5f / width, 0.0f };
	float y_offset_1[] = {  0.5f / width, 0.0f };
	float cbcr_offset0[] = { -1.0f / width, 0.0f };
	float cbcr_offset1[] = { -0.0f / width, 0.0f };
	set_uniform_vec2(uyvy_program_num, "foo", "luma_offset_0", y_offset_0);
	set_uniform_vec2(uyvy_program_num, "foo", "luma_offset_1", y_offset_1);
	set_uniform_vec2(uyvy_program_num, "foo", "chroma_offset_0", cbcr_offset0);
	set_uniform_vec2(uyvy_program_num, "foo", "chroma_offset_1", cbcr_offset1);

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	check_error();

	for (GLint attr_index : { uyvy_position_attribute_index, uyvy_texcoord_attribute_index }) {
		if (attr_index == -1) continue;
		glEnableVertexAttribArray(attr_index);
		check_error();
		glVertexAttribPointer(attr_index, 2, GL_FLOAT, GL_FALSE, 0, BUFFER_OFFSET(0));
		check_error();
	}

	glDrawArrays(GL_TRIANGLES, 0, 3);
	check_error();

	for (GLint attr_index : { uyvy_position_attribute_index, uyvy_texcoord_attribute_index }) {
		if (attr_index == -1) continue;
		glDisableVertexAttribArray(attr_index);
		check_error();
	}

	glActiveTexture(GL_TEXTURE0);
	check_error();
	glUseProgram(0);
	check_error();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	check_error();

	resource_pool->release_fbo(fbo);
	glDeleteVertexArrays(1, &vao);
}
