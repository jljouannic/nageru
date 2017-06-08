#include "metrics.h"

#include <locale>
#include <sstream>

using namespace std;

Metrics global_metrics;

void Metrics::register_int_metric(const string &name, atomic<int64_t> *location)
{
	lock_guard<mutex> lock(mu);
	int_metrics.emplace(name, location);
}

void Metrics::register_double_metric(const string &name, atomic<double> *location)
{
	lock_guard<mutex> lock(mu);
	double_metrics.emplace(name, location);
}

string Metrics::serialize() const
{
	stringstream ss;
	ss.imbue(locale("C"));
	ss.precision(20);
	ss << scientific;

	lock_guard<mutex> lock(mu);
	for (const auto &key_and_value : int_metrics) {
		ss << "nageru_" << key_and_value.first.c_str() << " " << key_and_value.second->load() << "\n";
	}
	for (const auto &key_and_value : double_metrics) {
		ss << "nageru_" << key_and_value.first.c_str() << " " << key_and_value.second->load() << "\n";
	}

	return ss.str();
}
