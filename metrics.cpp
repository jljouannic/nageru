#include "metrics.h"

#include <locale>
#include <sstream>

using namespace std;

Metrics global_metrics;

void Metrics::register_int_metric(const string &name, atomic<int64_t> *location, Metrics::Type type)
{
	lock_guard<mutex> lock(mu);
	int_metrics.emplace(name, Metric<int64_t>{ type, location });
}

void Metrics::register_double_metric(const string &name, atomic<double> *location, Metrics::Type type)
{
	lock_guard<mutex> lock(mu);
	double_metrics.emplace(name, Metric<double>{ type, location });
}

string Metrics::serialize() const
{
	stringstream ss;
	ss.imbue(locale("C"));
	ss.precision(20);
	ss << scientific;

	lock_guard<mutex> lock(mu);
	for (const auto &key_and_value : int_metrics) {
		if (key_and_value.second.type == TYPE_GAUGE) {
			ss << "# TYPE nageru_" << key_and_value.first << " gauge\n";
		}
		ss << "nageru_" << key_and_value.first << " " << key_and_value.second.location->load() << "\n";
	}
	for (const auto &key_and_value : double_metrics) {
		if (key_and_value.second.type == TYPE_GAUGE) {
			ss << "# TYPE nageru_" << key_and_value.first << " gauge\n";
		}
		ss << "nageru_" << key_and_value.first << " " << key_and_value.second.location->load() << "\n";
	}

	return ss.str();
}
