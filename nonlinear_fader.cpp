#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <vector>
#include <utility>

#include <QResizeEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>

#include "nonlinear_fader.h"

using namespace std;

namespace {

vector<pair<double, double>> fader_control_points = {
	// The main area is from +6 to -12 dB (18 dB), and we use half the slider range for it.
	{ 6.0, 0.0 },
	{ -12.0, 0.5 },

	// -12 to -21 is half the range (9 dB). Halve.
	{ -21.0, 0.625 },

	// -21 to -30 (9 dB) gets the same range as the previous one.
	{ -30.0, 0.75 },

	// -30 to -48 (18 dB) gets half of half.
	{ -48.0, 0.875 },

	// -48 to -84 (36 dB) gets half of half of half.
	{ -84.0, 1.0 },
};

double slider_fraction_to_db(double db)
{
	if (db <= fader_control_points[0].second) {
		return fader_control_points[0].first;
	}
	if (db >= fader_control_points.back().second) {
		return fader_control_points.back().first;
	}
	for (unsigned i = 1; i < fader_control_points.size(); ++i) {
		const double x0 = fader_control_points[i - 1].second;
		const double x1 = fader_control_points[i].second;
		const double y0 = fader_control_points[i - 1].first;
		const double y1 = fader_control_points[i].first;
		if (db >= x0 && db <= x1) {
			const double t = (db - x0) / (x1 - x0);
			return y0 + t * (y1 - y0);
		}
	}
	assert(false);
}

double db_to_slider_fraction(double x)
{
	if (x >= fader_control_points[0].first) {
		return fader_control_points[0].second;
	}
	if (x <= fader_control_points.back().first) {
		return fader_control_points.back().second;
	}
	for (unsigned i = 1; i < fader_control_points.size(); ++i) {
		const double x0 = fader_control_points[i].first;
		const double x1 = fader_control_points[i - 1].first;
		const double y0 = fader_control_points[i].second;
		const double y1 = fader_control_points[i - 1].second;
		if (x >= x0 && x <= x1) {
			const double t = (x - x0) / (x1 - x0);
			return y0 + t * (y1 - y0);
		}
	}
	assert(false);
}

}  // namespace

NonLinearFader::NonLinearFader(QWidget *parent)
	: QSlider(parent)
{
	update_slider_position();
}

void NonLinearFader::setDbValue(double db)
{
	db_value = db;
	update_slider_position();
	emit dbValueChanged(db);
}

void NonLinearFader::paintEvent(QPaintEvent *event)
{
	QStyleOptionSlider opt;
	this->initStyleOption(&opt);
	QRect gr = this->style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
	QRect sr = this->style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);

	// FIXME: Where does the slider_length / 2 come from? I can't really find it
	// in the Qt code, but it seems to match up with reality.
	int slider_length = sr.height();
	int slider_min = gr.top() + (slider_length / 2);
	int slider_max = gr.bottom() + (slider_length / 2) - slider_length + 1;

	QPainter p(this);

	// Draw some ticks every 6 dB.
	// FIXME: Find a way to make the slider wider, so that we have more space for tickmarks
	// and some dB numbering.
	int x_margin = 5;
	p.setPen(Qt::darkGray);
	for (int db = -84; db <= 6; db += 6) {
		int y = slider_min + lrint(db_to_slider_fraction(db) * (slider_max - slider_min));
		p.drawLine(QPoint(0, y), QPoint(gr.left() - x_margin, y));
		p.drawLine(QPoint(gr.right() + x_margin, y), QPoint(width() - 1, y));
	}

	QSlider::paintEvent(event);
}

void NonLinearFader::resizeEvent(QResizeEvent *event)
{
	QSlider::resizeEvent(event);
	inhibit_updates = true;
	setRange(0, event->size().height() - 1);
	inhibit_updates = false;
	update_slider_position();
}

void NonLinearFader::sliderChange(SliderChange change)
{
	QSlider::sliderChange(change);
	if (change == QAbstractSlider::SliderValueChange && !inhibit_updates) {
		if (value() == 0) {
			db_value = -HUGE_VAL;
		} else {
			double frac = 1.0 - value() / (height() - 1.0);
			db_value = slider_fraction_to_db(frac);
		}
		emit dbValueChanged(db_value);
	}
}

void NonLinearFader::update_slider_position()
{
	inhibit_updates = true;
	setValue(lrint((1.0 - db_to_slider_fraction(db_value)) * (height() - 1.0)));
	inhibit_updates = false;
}
