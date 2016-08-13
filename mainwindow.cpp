#include "mainwindow.h"

#include <math.h>
#include <stdio.h>
#include <signal.h>
#include <algorithm>
#include <chrono>
#include <string>
#include <vector>
#include <QBoxLayout>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QMetaType>
#include <QPushButton>
#include <QResizeEvent>
#include <QShortcut>
#include <QSize>
#include <QString>

#include "aboutdialog.h"
#include "disk_space_estimator.h"
#include "flags.h"
#include "glwidget.h"
#include "input_mapping_dialog.h"
#include "lrameter.h"
#include "mixer.h"
#include "post_to_main_thread.h"
#include "ui_audio_miniview.h"
#include "ui_display.h"
#include "ui_mainwindow.h"
#include "vumeter.h"

class QResizeEvent;

using namespace std;
using namespace std::chrono;
using namespace std::placeholders;

Q_DECLARE_METATYPE(std::string);
Q_DECLARE_METATYPE(std::vector<std::string>);

MainWindow *global_mainwindow = nullptr;

namespace {

void schedule_cut_signal(int ignored)
{
	global_mixer->schedule_cut();
}

void quit_signal(int ignored)
{
	global_mainwindow->close();
}

constexpr unsigned DB_NO_FLAGS = 0x0;
constexpr unsigned DB_WITH_SIGN = 0x1;
constexpr unsigned DB_BARE = 0x2;

string format_db(double db, unsigned flags)
{
	string text;
	if (flags & DB_WITH_SIGN) {
		if (isfinite(db)) {
			char buf[256];
			snprintf(buf, sizeof(buf), "%+.1f", db);
			text = buf;
		} else if (db < 0.0) {
			text = "-∞";
		} else {
			// Should never happen, really.
			text = "+∞";
		}
	} else {
		if (isfinite(db)) {
			char buf[256];
			snprintf(buf, sizeof(buf), "%.1f", db);
			text = buf;
		} else if (db < 0.0) {
			text = "-∞";
		} else {
			// Should never happen, really.
			text = "∞";
		}
	}
	if (!(flags & DB_BARE)) {
		text += " dB";
	}
	return text;
}

}  // namespace

MainWindow::MainWindow()
	: ui(new Ui::MainWindow)
{
	global_mainwindow = this;
	ui->setupUi(this);

	global_disk_space_estimator = new DiskSpaceEstimator(bind(&MainWindow::report_disk_space, this, _1, _2));
	disk_free_label = new QLabel(this);
	disk_free_label->setStyleSheet("QLabel {padding-right: 5px;}");
	ui->menuBar->setCornerWidget(disk_free_label);

	ui->me_live->set_output(Mixer::OUTPUT_LIVE);
	ui->me_preview->set_output(Mixer::OUTPUT_PREVIEW);

	// The menus.
	connect(ui->cut_action, &QAction::triggered, this, &MainWindow::cut_triggered);
	connect(ui->exit_action, &QAction::triggered, this, &MainWindow::exit_triggered);
	connect(ui->about_action, &QAction::triggered, this, &MainWindow::about_triggered);
	connect(ui->input_mapping_action, &QAction::triggered, this, &MainWindow::input_mapping_triggered);

	if (global_flags.x264_video_to_http) {
		connect(ui->x264_bitrate_action, &QAction::triggered, this, &MainWindow::x264_bitrate_triggered);
	} else {
		ui->x264_bitrate_action->setEnabled(false);
	}

	// Hook up the transition buttons. (Keyboard shortcuts are set in set_transition_names().)
	// TODO: Make them dynamic.
	connect(ui->transition_btn1, &QPushButton::clicked, bind(&MainWindow::transition_clicked, this, 0));
	connect(ui->transition_btn2, &QPushButton::clicked, bind(&MainWindow::transition_clicked, this, 1));
	connect(ui->transition_btn3, &QPushButton::clicked, bind(&MainWindow::transition_clicked, this, 2));

	// Aiee...
	transition_btn1 = ui->transition_btn1;
	transition_btn2 = ui->transition_btn2;
	transition_btn3 = ui->transition_btn3;
	qRegisterMetaType<string>("std::string");
	qRegisterMetaType<vector<string>>("std::vector<std::string>");
	connect(ui->me_live, &GLWidget::transition_names_updated, this, &MainWindow::set_transition_names);
	qRegisterMetaType<Mixer::Output>("Mixer::Output");

	last_audio_level_callback = steady_clock::now() - seconds(1);
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
	QMainWindow::resizeEvent(event);

	// Ask for a relayout, but only after the event loop is done doing relayout
	// on everything else.
	QMetaObject::invokeMethod(this, "relayout", Qt::QueuedConnection);
}

void MainWindow::mixer_created(Mixer *mixer)
{
	// Make the previews.
	unsigned num_previews = mixer->get_num_channels();

	for (unsigned i = 0; i < num_previews; ++i) {
		Mixer::Output output = Mixer::Output(Mixer::OUTPUT_INPUT0 + i);

		QWidget *preview = new QWidget(this);
		Ui::Display *ui_display = new Ui::Display;
		ui_display->setupUi(preview);
		ui_display->label->setText(mixer->get_channel_name(output).c_str());
		ui_display->display->set_output(output);
		ui->preview_displays->insertWidget(previews.size(), preview, 1);
		previews.push_back(ui_display);

		// Hook up the click.
		connect(ui_display->display, &GLWidget::clicked, bind(&MainWindow::channel_clicked, this, i));

		// Let the theme update the text whenever the resolution or color changed.
		connect(ui_display->display, &GLWidget::name_updated, this, &MainWindow::update_channel_name);
		connect(ui_display->display, &GLWidget::color_updated, this, &MainWindow::update_channel_color);

		// Hook up the keyboard key.
		QShortcut *shortcut = new QShortcut(QKeySequence(Qt::Key_1 + i), this);
		connect(shortcut, &QShortcut::activated, bind(&MainWindow::channel_clicked, this, i));

		// Hook up the white balance button (irrelevant if invisible).
		ui_display->wb_button->setVisible(mixer->get_supports_set_wb(output));
		connect(ui_display->wb_button, &QPushButton::clicked, bind(&MainWindow::wb_button_clicked, this, i));
	}

	setup_audio_miniview();

	// TODO: Fetch all of the values these for completeness,
	// not just the enable knobs implied by flags.
	ui->locut_enabled->setChecked(global_mixer->get_audio_mixer()->get_locut_enabled());
	ui->gainstaging_knob->setValue(global_mixer->get_audio_mixer()->get_gain_staging_db());
	ui->gainstaging_auto_checkbox->setChecked(global_mixer->get_audio_mixer()->get_gain_staging_auto());
	ui->compressor_enabled->setChecked(global_mixer->get_audio_mixer()->get_compressor_enabled());
	ui->limiter_enabled->setChecked(global_mixer->get_audio_mixer()->get_limiter_enabled());
	ui->makeup_gain_auto_checkbox->setChecked(global_mixer->get_audio_mixer()->get_final_makeup_gain_auto());

	ui->limiter_threshold_db_display->setText(
		QString::fromStdString(format_db(mixer->get_audio_mixer()->get_limiter_threshold_dbfs(), DB_WITH_SIGN)));
	ui->compressor_threshold_db_display->setText(
		QString::fromStdString(format_db(mixer->get_audio_mixer()->get_compressor_threshold_dbfs(), DB_WITH_SIGN)));

	connect(ui->locut_cutoff_knob, &QDial::valueChanged, this, &MainWindow::cutoff_knob_changed);
	cutoff_knob_changed(ui->locut_cutoff_knob->value());
	connect(ui->locut_enabled, &QCheckBox::stateChanged, [this](int state){
		global_mixer->get_audio_mixer()->set_locut_enabled(state == Qt::Checked);
	});

	connect(ui->gainstaging_knob, &QAbstractSlider::valueChanged, this, &MainWindow::gain_staging_knob_changed);
	connect(ui->gainstaging_auto_checkbox, &QCheckBox::stateChanged, [this](int state){
		global_mixer->get_audio_mixer()->set_gain_staging_auto(state == Qt::Checked);
	});
	connect(ui->makeup_gain_knob, &QAbstractSlider::valueChanged, this, &MainWindow::final_makeup_gain_knob_changed);
	connect(ui->makeup_gain_auto_checkbox, &QCheckBox::stateChanged, [this](int state){
		global_mixer->get_audio_mixer()->set_final_makeup_gain_auto(state == Qt::Checked);
	});

	connect(ui->limiter_threshold_knob, &QDial::valueChanged, this, &MainWindow::limiter_threshold_knob_changed);
	connect(ui->compressor_threshold_knob, &QDial::valueChanged, this, &MainWindow::compressor_threshold_knob_changed);
	connect(ui->limiter_enabled, &QCheckBox::stateChanged, [this](int state){
		global_mixer->get_audio_mixer()->set_limiter_enabled(state == Qt::Checked);
	});
	connect(ui->compressor_enabled, &QCheckBox::stateChanged, [this](int state){
		global_mixer->get_audio_mixer()->set_compressor_enabled(state == Qt::Checked);
	});
	connect(ui->reset_meters_button, &QPushButton::clicked, this, &MainWindow::reset_meters_button_clicked);
	mixer->get_audio_mixer()->set_audio_level_callback(bind(&MainWindow::audio_level_callback, this, _1, _2, _3, _4, _5, _6, _7, _8, _9));

	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = schedule_cut_signal;
	act.sa_flags = SA_RESTART;
	sigaction(SIGHUP, &act, nullptr);

	// Mostly for debugging. Don't override SIGINT, that's so evil if
	// shutdown isn't instant.
	memset(&act, 0, sizeof(act));
	act.sa_handler = quit_signal;
	act.sa_flags = SA_RESTART;
	sigaction(SIGUSR1, &act, nullptr);
}

void MainWindow::setup_audio_miniview()
{
	// Remove any existing channels.
	for (QLayoutItem *item; (item = ui->faders->takeAt(0)) != nullptr; ) {
		delete item->widget();
		delete item;
	}
	audio_miniviews.clear();

	// Set up brand new ones from the input mapping.
	InputMapping mapping = global_mixer->get_audio_mixer()->get_input_mapping();
	audio_miniviews.resize(mapping.buses.size());
	for (unsigned bus_index = 0; bus_index < mapping.buses.size(); ++bus_index) {
		QWidget *channel = new QWidget(this);
		Ui::AudioMiniView *ui_audio_miniview = new Ui::AudioMiniView;
		ui_audio_miniview->setupUi(channel);
		ui_audio_miniview->bus_desc_label->setFullText(
			QString::fromStdString(mapping.buses[bus_index].name));
		audio_miniviews[bus_index] = ui_audio_miniview;
		// TODO: Set the fader position.
		ui->faders->addWidget(channel);

		connect(ui_audio_miniview->fader, &QAbstractSlider::valueChanged,
			bind(&MainWindow::mini_fader_changed, this, ui_audio_miniview, bus_index, _1));
	}
}

void MainWindow::mixer_shutting_down()
{
	ui->me_live->clean_context();
	ui->me_preview->clean_context();
	for (Ui::Display *display : previews) {
		display->display->clean_context();
	}
}

void MainWindow::cut_triggered()
{
	global_mixer->schedule_cut();
}

void MainWindow::x264_bitrate_triggered()
{
	bool ok;
	int new_bitrate = QInputDialog::getInt(this, "Change x264 bitrate", "Choose new bitrate for x264 HTTP output (from 100–100,000 kbit/sec):", global_flags.x264_bitrate, /*min=*/100, /*max=*/100000, /*step=*/100, &ok);
	if (ok && new_bitrate >= 100 && new_bitrate <= 100000) {
		global_flags.x264_bitrate = new_bitrate;
		global_mixer->change_x264_bitrate(new_bitrate);
	}
}

void MainWindow::exit_triggered()
{
	close();
}

void MainWindow::about_triggered()
{
	AboutDialog().exec();
}

void MainWindow::input_mapping_triggered()
{
	if (InputMappingDialog().exec() == QDialog::Accepted) {
		setup_audio_miniview();
	}
}

void MainWindow::gain_staging_knob_changed(int value)
{
	ui->gainstaging_auto_checkbox->setCheckState(Qt::Unchecked);

	float gain_db = value * 0.1f;
	global_mixer->get_audio_mixer()->set_gain_staging_db(gain_db);

	// The label will be updated by the audio level callback.
}

void MainWindow::final_makeup_gain_knob_changed(int value)
{
	ui->makeup_gain_auto_checkbox->setCheckState(Qt::Unchecked);

	float gain_db = value * 0.1f;
	global_mixer->get_audio_mixer()->set_final_makeup_gain_db(gain_db);

	// The label will be updated by the audio level callback.
}

void MainWindow::cutoff_knob_changed(int value)
{
	float octaves = value * 0.1f;
	float cutoff_hz = 20.0 * pow(2.0, octaves);
	global_mixer->get_audio_mixer()->set_locut_cutoff(cutoff_hz);

	char buf[256];
	snprintf(buf, sizeof(buf), "%ld Hz", lrintf(cutoff_hz));
	ui->locut_cutoff_display->setText(buf);
}

void MainWindow::report_disk_space(off_t free_bytes, double estimated_seconds_left)
{
	char time_str[256];
	if (estimated_seconds_left < 60.0) {
		strcpy(time_str, "<font color=\"red\">Less than a minute</font>");
	} else if (estimated_seconds_left < 1800.0) {  // Less than half an hour: Xm Ys (red).
		int s = lrintf(estimated_seconds_left);
		int m = s / 60;
		s %= 60;
		snprintf(time_str, sizeof(time_str), "<font color=\"red\">%dm %ds</font>", m, s);
	} else if (estimated_seconds_left < 3600.0) {  // Less than an hour: Xm.
		int m = lrintf(estimated_seconds_left / 60.0);
		snprintf(time_str, sizeof(time_str), "%dm", m);
	} else if (estimated_seconds_left < 36000.0) {  // Less than ten hours: Xh Ym.
		int m = lrintf(estimated_seconds_left / 60.0);
		int h = m / 60;
		m %= 60;
		snprintf(time_str, sizeof(time_str), "%dh %dm", h, m);
	} else {  // More than ten hours: Xh.
		int h = lrintf(estimated_seconds_left / 3600.0);
		snprintf(time_str, sizeof(time_str), "%dh", h);
	}
	char buf[256];
	snprintf(buf, sizeof(buf), "Disk free: %'.0f MB (approx. %s)", free_bytes / 1048576.0, time_str);

	std::string label = buf;

	post_to_main_thread([this, label]{
		disk_free_label->setText(QString::fromStdString(label));
		ui->menuBar->setCornerWidget(disk_free_label);  // Need to set this again for the sizing to get right.
	});
}

void MainWindow::limiter_threshold_knob_changed(int value)
{
	float threshold_dbfs = value * 0.1f;
	global_mixer->get_audio_mixer()->set_limiter_threshold_dbfs(threshold_dbfs);
	ui->limiter_threshold_db_display->setText(
		QString::fromStdString(format_db(threshold_dbfs, DB_WITH_SIGN)));
}

void MainWindow::compressor_threshold_knob_changed(int value)
{
	float threshold_dbfs = value * 0.1f;
	global_mixer->get_audio_mixer()->set_compressor_threshold_dbfs(threshold_dbfs);
	ui->compressor_threshold_db_display->setText(
		QString::fromStdString(format_db(threshold_dbfs, DB_WITH_SIGN)));
}

void MainWindow::mini_fader_changed(Ui::AudioMiniView *ui, int channel, int value)
{
	float volume_db = value * 0.1f;

	char buf[256];
	snprintf(buf, sizeof(buf), "%+.1f dB", volume_db);
	ui->fader_label->setText(buf);

	global_mixer->get_audio_mixer()->set_fader_volume(channel, volume_db);
}

void MainWindow::reset_meters_button_clicked()
{
	global_mixer->get_audio_mixer()->reset_meters();
	ui->peak_display->setText(QString::fromStdString(format_db(-HUGE_VAL, DB_WITH_SIGN | DB_BARE)));
	ui->peak_display->setStyleSheet("");
}

void MainWindow::audio_level_callback(float level_lufs, float peak_db, vector<float> bus_level_lufs,
                                      float global_level_lufs,
                                      float range_low_lufs, float range_high_lufs,
                                      float gain_staging_db, float final_makeup_gain_db,
                                      float correlation)
{
	steady_clock::time_point now = steady_clock::now();

	// The meters are somewhat inefficient to update. Only update them
	// every 100 ms or so (we get updates every 5–20 ms).
	double last_update_age = duration<double>(now - last_audio_level_callback).count();
	if (last_update_age < 0.100) {
		return;
	}
	last_audio_level_callback = now;

	post_to_main_thread([=]() {
		ui->vu_meter->set_level(level_lufs);
		for (unsigned bus_index = 0; bus_index < bus_level_lufs.size(); ++bus_index) {
			if (bus_index < audio_miniviews.size()) {
				audio_miniviews[bus_index]->vu_meter_meter->set_level(
					bus_level_lufs[bus_index]);
			}
		}
		ui->lra_meter->set_levels(global_level_lufs, range_low_lufs, range_high_lufs);
		ui->correlation_meter->set_correlation(correlation);

		ui->peak_display->setText(QString::fromStdString(format_db(peak_db, DB_BARE)));
		if (peak_db > -0.1f) {  // -0.1 dBFS is EBU peak limit.
			ui->peak_display->setStyleSheet("QLabel { background-color: red; color: white; }");
		} else {
			ui->peak_display->setStyleSheet("");
		}

		ui->gainstaging_knob->blockSignals(true);
		ui->gainstaging_knob->setValue(lrintf(gain_staging_db * 10.0f));
		ui->gainstaging_knob->blockSignals(false);
		ui->gainstaging_db_display->setText(
			QString::fromStdString(format_db(gain_staging_db, DB_WITH_SIGN)));

		ui->makeup_gain_knob->blockSignals(true);
		ui->makeup_gain_knob->setValue(lrintf(final_makeup_gain_db * 10.0f));
		ui->makeup_gain_knob->blockSignals(false);
		ui->makeup_gain_db_display->setText(
			QString::fromStdString(format_db(final_makeup_gain_db, DB_WITH_SIGN)));
	});
}

void MainWindow::relayout()
{
	int height = ui->vertical_layout->geometry().height();

	double remaining_height = height;

	// Allocate the height; the most important part is to keep the main displays
	// at 16:9 if at all possible.
	double me_width = ui->me_preview->width();
	double me_height = me_width * 9.0 / 16.0 + ui->label_preview->height() + ui->preview_vertical_layout->spacing();

	// TODO: Scale the widths when we need to do this.
	if (me_height / double(height) > 0.8) {
		me_height = height * 0.8;
	}
	remaining_height -= me_height + ui->vertical_layout->spacing();

	// Space between the M/E displays and the audio strip.
	remaining_height -= ui->vertical_layout->spacing();

	// The previews will be constrained by the remaining height, and the width.
	double preview_label_height = previews[0]->title_bar->geometry().height() +
		previews[0]->main_vertical_layout->spacing();
	int preview_total_width = ui->preview_displays->geometry().width() - (previews.size() - 1) * ui->preview_displays->spacing();
	double preview_height = min(remaining_height - preview_label_height, (preview_total_width / double(previews.size())) * 9.0 / 16.0);
	remaining_height -= preview_height + preview_label_height + ui->vertical_layout->spacing();

	ui->vertical_layout->setStretch(0, lrintf(me_height));
	ui->vertical_layout->setStretch(1, lrintf(remaining_height));  // Audio strip.
	ui->vertical_layout->setStretch(2, lrintf(preview_height + preview_label_height));

	// Set the widths for the previews.
	double preview_width = preview_height * 16.0 / 9.0;
	for (unsigned i = 0; i < previews.size(); ++i) {
		ui->preview_displays->setStretch(i, lrintf(preview_width));
	}

	// The preview horizontal spacer.
	double remaining_preview_width = preview_total_width - previews.size() * preview_width;
	ui->preview_displays->setStretch(previews.size(), lrintf(remaining_preview_width));
}

void MainWindow::set_transition_names(vector<string> transition_names)
{
	if (transition_names.size() < 1 || transition_names[0].empty()) {
		transition_btn1->setText(QString(""));
	} else {
		transition_btn1->setText(QString::fromStdString(transition_names[0] + " (J)"));
		ui->transition_btn1->setShortcut(QKeySequence("J"));
	}
	if (transition_names.size() < 2 || transition_names[1].empty()) {
		transition_btn2->setText(QString(""));
	} else {
		transition_btn2->setText(QString::fromStdString(transition_names[1] + " (K)"));
		ui->transition_btn2->setShortcut(QKeySequence("K"));
	}
	if (transition_names.size() < 3 || transition_names[2].empty()) {
		transition_btn3->setText(QString(""));
	} else {
		transition_btn3->setText(QString::fromStdString(transition_names[2] + " (L)"));
		ui->transition_btn3->setShortcut(QKeySequence("L"));
	}
}

void MainWindow::update_channel_name(Mixer::Output output, const string &name)
{
	if (output >= Mixer::OUTPUT_INPUT0) {
		unsigned channel = output - Mixer::OUTPUT_INPUT0;
		previews[channel]->label->setText(name.c_str());
	}
}

void MainWindow::update_channel_color(Mixer::Output output, const string &color)
{
	if (output >= Mixer::OUTPUT_INPUT0) {
		unsigned channel = output - Mixer::OUTPUT_INPUT0;
		previews[channel]->frame->setStyleSheet(QString::fromStdString("background-color:" + color));
	}
}

void MainWindow::transition_clicked(int transition_number)
{
	global_mixer->transition_clicked(transition_number);
}

void MainWindow::channel_clicked(int channel_number)
{
	if (current_wb_pick_display == channel_number) {
		// The picking was already done from eventFilter(), since we don't get
		// the mouse pointer here.
	} else {
		global_mixer->channel_clicked(channel_number);
	}
}

void MainWindow::wb_button_clicked(int channel_number)
{
	current_wb_pick_display = channel_number;
	QApplication::setOverrideCursor(Qt::CrossCursor);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
	if (current_wb_pick_display != -1 &&
	    event->type() == QEvent::MouseButtonRelease &&
	    watched->isWidgetType()) {
		QApplication::restoreOverrideCursor();
		if (watched == previews[current_wb_pick_display]->display) {
			const QMouseEvent *mouse_event = (QMouseEvent *)event;
			set_white_balance(current_wb_pick_display, mouse_event->x(), mouse_event->y());
		} else {
			// The user clicked on something else, give up.
			// (The click goes through, which might not be ideal, but, yes.)
			current_wb_pick_display = -1;
		}
	}
	return false;
}

namespace {

double srgb_to_linear(double x)
{
	if (x < 0.04045) {
		return x / 12.92;
	} else {
		return pow((x + 0.055) / 1.055, 2.4);
	}
}

}  // namespace

void MainWindow::set_white_balance(int channel_number, int x, int y)
{
	// Set the white balance to neutral for the grab. It's probably going to
	// flicker a bit, but hopefully this display is not live anyway.
	global_mixer->set_wb(Mixer::OUTPUT_INPUT0 + channel_number, 0.5, 0.5, 0.5);
	previews[channel_number]->display->updateGL();
	QRgb reference_color = previews[channel_number]->display->grabFrameBuffer().pixel(x, y);

	double r = srgb_to_linear(qRed(reference_color) / 255.0);
	double g = srgb_to_linear(qGreen(reference_color) / 255.0);
	double b = srgb_to_linear(qBlue(reference_color) / 255.0);
	global_mixer->set_wb(Mixer::OUTPUT_INPUT0 + channel_number, r, g, b);
	previews[channel_number]->display->updateGL();
}
