#include "vu_common.h"

#include <math.h>
#include <algorithm>

#include <QBrush>
#include <QColor>
#include <QPainter>

using namespace std;

double lufs_to_pos(float level_lu, int height)
{       
	const float min_level = 9.0f;    // y=0 is top of screen, so “min” is the loudest level.
	const float max_level = -18.0f;

	// Handle -inf.
	if (level_lu < max_level) {
		return height - 1;
	}

	double y = height * (level_lu - min_level) / (max_level - min_level);
	y = max<double>(y, 0);
	y = min<double>(y, height - 1);

	// If we are big enough, snap to pixel grid instead of antialiasing
	// the edges; the unevenness will be less noticeable than the blurriness.
	double height_per_level = height / (min_level - max_level) - 2.0;
	if (height_per_level >= 10.0) {
		y = round(y);
	}

	return y;
}

void draw_vu_meter(QPainter &painter, int width, int height, int margin, bool is_on)
{
	painter.fillRect(margin, 0, width - 2 * margin, height, Qt::black);

	for (int y = 0; y < height; ++y) {
		// Find coverage of “on” rectangles in this pixel row.
		double coverage = 0.0;
		for (int level = -18; level < 9; ++level) {
			double min_y = lufs_to_pos(level + 1.0, height) + 1.0;
			double max_y = lufs_to_pos(level, height) - 1.0;
			min_y = std::max<double>(min_y, y);
			min_y = std::min<double>(min_y, y + 1);
			max_y = std::max<double>(max_y, y);
			max_y = std::min<double>(max_y, y + 1);
			coverage += max_y - min_y;
		}

		double on_r, on_g, on_b;
		if (is_on) {
			double t = double(y) / height;
			if (t <= 0.5) {
				on_r = 1.0;
				on_g = 2.0 * t;
				on_b = 0.0;
			} else {
				on_r = 1.0 - 2.0 * (t - 0.5);
				on_g = 1.0;
				on_b = 0.0;
			}
		} else {
			on_r = on_g = on_b = 0.05;
		}

		// Correct for coverage and do a simple gamma correction.
		int r = lrintf(255 * pow(on_r * coverage, 1.0 / 2.2));
		int g = lrintf(255 * pow(on_g * coverage, 1.0 / 2.2));
		int b = lrintf(255 * pow(on_b * coverage, 1.0 / 2.2));
		painter.fillRect(margin, y, width - 2 * margin, 1, QColor(r, g, b));
	}
}
