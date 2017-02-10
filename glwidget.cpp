#include "glwidget.h"

#include <assert.h>
#include <bmusb/bmusb.h>
#include <movit/effect_chain.h>
#include <movit/resource_pool.h>
#include <stdbool.h>
#include <stdint.h>
#include <QAction>
#include <QActionGroup>
#include <QList>
#include <QMenu>
#include <QPoint>
#include <QVariant>
#include <QWidget>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

#include "audio_mixer.h"
#include "context.h"
#include "flags.h"
#include "mainwindow.h"
#include "mixer.h"
#include "ref_counted_gl_sync.h"

class QMouseEvent;

#undef Success
#include <movit/util.h>
#include <string>

using namespace std;
using namespace std::placeholders;

GLWidget::GLWidget(QWidget *parent)
    : QGLWidget(parent, global_share_widget)
{
}

void GLWidget::clean_context()
{
	if (resource_pool != nullptr) {
		makeCurrent();
		resource_pool->clean_context();
	}
}

void GLWidget::initializeGL()
{
	static once_flag flag;
	call_once(flag, [this]{
		global_mixer = new Mixer(QGLFormat::toSurfaceFormat(format()), global_flags.num_cards);
		global_audio_mixer = global_mixer->get_audio_mixer();
		global_mainwindow->mixer_created(global_mixer);
		global_mixer->start();
	});
	global_mixer->set_frame_ready_callback(output, [this]{
		QMetaObject::invokeMethod(this, "update", Qt::AutoConnection);
	});
	if (output == Mixer::OUTPUT_LIVE) {
		global_mixer->set_transition_names_updated_callback(output, [this](const vector<string> &names){
			emit transition_names_updated(names);
		});
		setContextMenuPolicy(Qt::CustomContextMenu);
		connect(this, &QWidget::customContextMenuRequested,
			bind(&GLWidget::show_live_context_menu, this, _1));
	}
	if (output >= Mixer::OUTPUT_INPUT0) {
		global_mixer->set_name_updated_callback(output, [this](const string &name){
			emit name_updated(output, name);
		});
		global_mixer->set_color_updated_callback(output, [this](const string &color){
			emit color_updated(output, color);
		});

		int signal_num = global_mixer->get_channel_signal(output);
		if (signal_num != -1) {
			setContextMenuPolicy(Qt::CustomContextMenu);
			connect(this, &QWidget::customContextMenuRequested,
			        bind(&GLWidget::show_preview_context_menu, this, signal_num, _1));
		}
	}

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDepthMask(GL_FALSE);
}

void GLWidget::resizeGL(int width, int height)
{
	glViewport(0, 0, width, height);
}

void GLWidget::paintGL()
{
	Mixer::DisplayFrame frame;
	if (!global_mixer->get_display_frame(output, &frame)) {
		glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
		check_error();
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		check_error();
		return;
	}

	check_error();
	glWaitSync(frame.ready_fence.get(), /*flags=*/0, GL_TIMEOUT_IGNORED);
	check_error();
	frame.setup_chain();
	check_error();
	frame.chain->render_to_screen();
	check_error();

	if (resource_pool == nullptr) {
		resource_pool = frame.chain->get_resource_pool();
	} else {
		assert(resource_pool == frame.chain->get_resource_pool());
	}
}

void GLWidget::mousePressEvent(QMouseEvent *event)
{
	emit clicked();
}

void GLWidget::show_live_context_menu(const QPoint &pos)
{
	QPoint global_pos = mapToGlobal(pos);

	QMenu menu;

	// Add a submenu for selecting output card, with an action for each card.
	QMenu card_submenu;
	QActionGroup card_group(&card_submenu);

	int current_card = global_mixer->get_output_card_index();

	QAction *none_action = new QAction("None", &card_group);
	none_action->setCheckable(true);
	if (current_card == -1) {
		none_action->setChecked(true);
	}
	none_action->setData(QList<QVariant>{"output_card", -1});
	card_submenu.addAction(none_action);

	unsigned num_cards = global_mixer->get_num_cards();
	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		if (!global_mixer->card_can_be_used_as_output(card_index)) {
			continue;
		}

		QString description(QString::fromStdString(global_mixer->get_output_card_description(card_index)));
		QAction *action = new QAction(description, &card_group);
		action->setCheckable(true);
		if (current_card == int(card_index)) {
			action->setChecked(true);
		}
		action->setData(QList<QVariant>{"output_card", card_index});
		card_submenu.addAction(action);
	}

	card_submenu.setTitle("HDMI/SDI output");
	menu.addMenu(&card_submenu);

	// Add a submenu for choosing the output resolution. Since this is
	// card-dependent, it is disabled if we haven't chosen a card
	// (but it's still there so that the user will know it exists).
	QMenu resolution_submenu;
	QActionGroup resolution_group(&resolution_submenu);
	if (current_card == -1) {
		resolution_submenu.setEnabled(false);
	} else {
		uint32_t current_mode = global_mixer->get_output_video_mode();
		map<uint32_t, bmusb::VideoMode> video_modes = global_mixer->get_available_output_video_modes();
		for (const auto &mode : video_modes) {
			QString description(QString::fromStdString(mode.second.name));
			QAction *action = new QAction(description, &resolution_group);
			action->setCheckable(true);
			if (current_mode == mode.first) {
				action->setChecked(true);
			}
			action->setData(QList<QVariant>{"video_mode", mode.first});
			resolution_submenu.addAction(action);
		}
	}

	resolution_submenu.setTitle("HDMI/SDI output resolution");
	menu.addMenu(&resolution_submenu);

	// Show the menu and look at the result.
	QAction *selected_item = menu.exec(global_pos);
	if (selected_item != nullptr) {
		QList<QVariant> selected = selected_item->data().toList();
		if (selected[0].toString() == "output_card") {
			unsigned output_card = selected[1].toUInt(nullptr);
			global_mixer->set_output_card(output_card);
		} else if (selected[0].toString() == "video_mode") {
			uint32_t mode = selected[1].toUInt(nullptr);
			global_mixer->set_output_video_mode(mode);
		} else {
			assert(false);
		}
	}
}

void GLWidget::show_preview_context_menu(unsigned signal_num, const QPoint &pos)
{
	QPoint global_pos = mapToGlobal(pos);

	QMenu menu;

	// Add a submenu for selecting input card, with an action for each card.
	QMenu card_submenu;
	QActionGroup card_group(&card_submenu);

	unsigned num_cards = global_mixer->get_num_cards();
	unsigned current_card = global_mixer->map_signal(signal_num);
	for (unsigned card_index = 0; card_index < num_cards; ++card_index) {
		QString description(QString::fromStdString(global_mixer->get_card_description(card_index)));
		QAction *action = new QAction(description, &card_group);
		action->setCheckable(true);
		if (current_card == card_index) {
			action->setChecked(true);
		}
		action->setData(QList<QVariant>{"card", card_index});
		card_submenu.addAction(action);
	}

	card_submenu.setTitle("Input source");
	menu.addMenu(&card_submenu);

	// --- The choices in the next few options depend a lot on which card is active ---

	// Add a submenu for selecting video input, with an action for each input.
	QMenu video_input_submenu;
	QActionGroup video_input_group(&video_input_submenu);
	std::map<uint32_t, string> video_inputs = global_mixer->get_available_video_inputs(current_card);
	uint32_t current_video_input = global_mixer->get_current_video_input(current_card);
	for (const auto &mode : video_inputs) {
		QString description(QString::fromStdString(mode.second));
		QAction *action = new QAction(description, &video_input_group);
		action->setCheckable(true);
		if (mode.first == current_video_input) {
			action->setChecked(true);
		}
		action->setData(QList<QVariant>{"video_input", mode.first});
		video_input_submenu.addAction(action);
	}

	video_input_submenu.setTitle("Video input");
	menu.addMenu(&video_input_submenu);

	// The same for audio input.
	QMenu audio_input_submenu;
	QActionGroup audio_input_group(&audio_input_submenu);
	std::map<uint32_t, string> audio_inputs = global_mixer->get_available_audio_inputs(current_card);
	uint32_t current_audio_input = global_mixer->get_current_audio_input(current_card);
	for (const auto &mode : audio_inputs) {
		QString description(QString::fromStdString(mode.second));
		QAction *action = new QAction(description, &audio_input_group);
		action->setCheckable(true);
		if (mode.first == current_audio_input) {
			action->setChecked(true);
		}
		action->setData(QList<QVariant>{"audio_input", mode.first});
		audio_input_submenu.addAction(action);
	}

	audio_input_submenu.setTitle("Audio input");
	menu.addMenu(&audio_input_submenu);

	// The same for resolution.
	QMenu mode_submenu;
	QActionGroup mode_group(&mode_submenu);
	std::map<uint32_t, bmusb::VideoMode> video_modes = global_mixer->get_available_video_modes(current_card);
	uint32_t current_video_mode = global_mixer->get_current_video_mode(current_card);
	bool has_auto_mode = false;
	for (const auto &mode : video_modes) {
		QString description(QString::fromStdString(mode.second.name));
		QAction *action = new QAction(description, &mode_group);
		action->setCheckable(true);
		if (mode.first == current_video_mode) {
			action->setChecked(true);
		}
		action->setData(QList<QVariant>{"video_mode", mode.first});
		mode_submenu.addAction(action);

		// TODO: Relying on the 0 value here (from bmusb.h) is ugly, it should be a named constant.
		if (mode.first == 0) {
			has_auto_mode = true;
		}
	}

	// Add a “scan” menu if there's no “auto” mode.
	if (!has_auto_mode) {
		QAction *action = new QAction("Scan", &mode_group);
		action->setData(QList<QVariant>{"video_mode", 0});
		mode_submenu.addSeparator();
		mode_submenu.addAction(action);
	}

	mode_submenu.setTitle("Input mode");
	menu.addMenu(&mode_submenu);

	// --- End of card-dependent choices ---

	// Add an audio source selector.
	QAction *audio_source_action = nullptr;
	if (global_audio_mixer->get_mapping_mode() == AudioMixer::MappingMode::SIMPLE) {
		audio_source_action = new QAction("Use as audio source", &menu);
		audio_source_action->setCheckable(true);
		if (global_audio_mixer->get_simple_input() == signal_num) {
			audio_source_action->setChecked(true);
			audio_source_action->setEnabled(false);
		}
		menu.addAction(audio_source_action);
	}

	// And a master clock selector.
	QAction *master_clock_action = new QAction("Use as master clock", &menu);
	master_clock_action->setCheckable(true);
	if (global_mixer->get_output_card_index() != -1) {
		master_clock_action->setChecked(false);
		master_clock_action->setEnabled(false);
	} else if (global_mixer->get_master_clock() == signal_num) {
		master_clock_action->setChecked(true);
		master_clock_action->setEnabled(false);
	}
	menu.addAction(master_clock_action);

	// Show the menu and look at the result.
	QAction *selected_item = menu.exec(global_pos);
	if (audio_source_action != nullptr && selected_item == audio_source_action) {
		global_audio_mixer->set_simple_input(signal_num);
	} else if (selected_item == master_clock_action) {
		global_mixer->set_master_clock(signal_num);
	} else if (selected_item != nullptr) {
		QList<QVariant> selected = selected_item->data().toList();
		if (selected[0].toString() == "video_mode") {
			uint32_t mode = selected[1].toUInt(nullptr);
			if (mode == 0 && !has_auto_mode) {
				global_mixer->start_mode_scanning(current_card);
			} else {
				global_mixer->set_video_mode(current_card, mode);
			}
		} else if (selected[0].toString() == "video_input") {
			uint32_t input = selected[1].toUInt(nullptr);
			global_mixer->set_video_input(current_card, input);
		} else if (selected[0].toString() == "audio_input") {
			uint32_t input = selected[1].toUInt(nullptr);
			global_mixer->set_audio_input(current_card, input);
		} else if (selected[0].toString() == "card") {
			unsigned card_index = selected[1].toUInt(nullptr);
			global_mixer->set_signal_mapping(signal_num, card_index);
		} else {
			assert(false);
		}
	}
}
