#include "analyzer.h"

#include <QDialogButtonBox>
#include <QSurface>

#include <movit/resource_pool.h>
#include <movit/util.h>

#include "context.h"
#include "flags.h"
#include "mixer.h"
#include "ui_analyzer.h"

using namespace std;

Analyzer::Analyzer()
	: ui(new Ui::Analyzer)
{
	ui->setupUi(this);

	//connect(ui->button_box, &QDialogButtonBox::accepted, [this]{ this->close(); });

	ui->input_box->addItem("Live", Mixer::OUTPUT_LIVE);
	ui->input_box->addItem("Preview", Mixer::OUTPUT_PREVIEW);
	unsigned num_channels = global_mixer->get_num_channels();
	for (unsigned channel_idx = 0; channel_idx < num_channels; ++channel_idx) {
		Mixer::Output channel = static_cast<Mixer::Output>(Mixer::OUTPUT_INPUT0 + channel_idx);	
		string name = global_mixer->get_channel_name(channel);
		ui->input_box->addItem(QString::fromStdString(name), channel);
	}

	connect(ui->grab_btn, &QPushButton::clicked, bind(&Analyzer::grab_clicked, this));
	connect(ui->input_box, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged), bind(&Analyzer::signal_changed, this));
	signal_changed();

	surface = create_surface(QSurfaceFormat::defaultFormat());
	context = create_context(surface);

	if (!make_current(context, surface)) {
		printf("oops\n");
		exit(1);
	}

        glGenBuffers(1, &pbo);
        glBindBuffer(GL_PIXEL_PACK_BUFFER_ARB, pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER_ARB, global_flags.width * global_flags.height * 4, NULL, GL_STREAM_READ);
}

Analyzer::~Analyzer()
{
	if (!make_current(context, surface)) {
		printf("oops\n");
		exit(1);
	}
	glDeleteBuffers(1, &pbo);
	check_error();
	if (resource_pool != nullptr) {
		resource_pool->clean_context();
	}
	delete_context(context);
	delete surface;  // TODO?
}

void Analyzer::grab_clicked()
{
	Mixer::Output channel = static_cast<Mixer::Output>(ui->input_box->currentData().value<int>());

	if (!make_current(context, surface)) {
		printf("oops\n");
		exit(1);
	}

	Mixer::DisplayFrame frame;
	if (!global_mixer->get_display_frame(channel, &frame)) {
		printf("Not ready yet\n");
		return;
	}

	// Set up an FBO to render into.
	if (resource_pool == nullptr) {
		resource_pool = frame.chain->get_resource_pool();
	} else {
		assert(resource_pool == frame.chain->get_resource_pool());
	}
	GLuint fbo_tex = resource_pool->create_2d_texture(GL_RGBA8, global_flags.width, global_flags.height);
	check_error();
	GLuint fbo = resource_pool->create_fbo(fbo_tex);
	check_error();

	glWaitSync(frame.ready_fence.get(), /*flags=*/0, GL_TIMEOUT_IGNORED);
	check_error();
	frame.setup_chain();
	check_error();
	glDisable(GL_FRAMEBUFFER_SRGB);
	check_error();
	frame.chain->render_to_fbo(fbo, global_flags.width, global_flags.height);
	check_error();

	// Read back to memory.
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	check_error();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo);
	check_error();
	glReadPixels(0, 0, global_flags.width, global_flags.height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, BUFFER_OFFSET(0));
	check_error();

	unsigned char *buf = (unsigned char *)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
	check_error();

	int r_hist[256] = {0}, g_hist[256] = {0}, b_hist[256] = {0};
	const unsigned char *ptr = buf;
	for (int y = 0; y < global_flags.width; ++y) {
		for (int x = 0; x < global_flags.height; ++x) {
			uint8_t b = *ptr++;
			uint8_t g = *ptr++;
			uint8_t r = *ptr++;
			uint8_t a = *ptr++;

			++r_hist[r];
			++g_hist[g];
			++b_hist[b];
		}
	}

	glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
	check_error();
	glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	check_error();
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	check_error();

	printf("R hist:");
	for (unsigned i = 0; i < 256; ++i) { printf(" %d", r_hist[i]); }
	printf("\n");
	printf("G hist:");
	for (unsigned i = 0; i < 256; ++i) { printf(" %d", g_hist[i]); }
	printf("\n");
	printf("B hist:");
	for (unsigned i = 0; i < 256; ++i) { printf(" %d", b_hist[i]); }
	printf("\n");

	resource_pool->release_2d_texture(fbo_tex);
	check_error();
	resource_pool->release_fbo(fbo);
	check_error();
}

void Analyzer::signal_changed()
{
	Mixer::Output channel = static_cast<Mixer::Output>(ui->input_box->currentData().value<int>());
	ui->display->set_output(channel);
}
