#include "metrics.h"

#include <assert.h>

#include <locale>
#include <sstream>

using namespace std;

Metrics global_metrics;

void Metrics::add(const string &name, const vector<pair<string, string>> &labels, atomic<int64_t> *location, Metrics::Type type)
{
	Metric metric;
	metric.data_type = DATA_TYPE_INT64;
	metric.name = name;
	metric.labels = labels;
	metric.location_int64 = location;

	lock_guard<mutex> lock(mu);
	metrics.push_back(metric);
	assert(types.count(name) == 0 || types[name] == type);
	types[name] = type;
}

void Metrics::add(const string &name, const vector<pair<string, string>> &labels, atomic<double> *location, Metrics::Type type)
{
	Metric metric;
	metric.data_type = DATA_TYPE_DOUBLE;
	metric.name = name;
	metric.labels = labels;
	metric.location_double = location;

	lock_guard<mutex> lock(mu);
	metrics.push_back(metric);
	assert(types.count(name) == 0 || types[name] == type);
	types[name] = type;
}

string Metrics::serialize() const
{
	stringstream ss;
	ss.imbue(locale("C"));
	ss.precision(20);

	lock_guard<mutex> lock(mu);
	for (const auto &name_and_type : types) {
		if (name_and_type.second == TYPE_GAUGE) {
			ss << "# TYPE nageru_" << name_and_type.first << " gauge\n";
		}
	}
	for (const Metric &metric : metrics) {
		string name;
		if (metric.labels.empty()) {
			name = "nageru_" + metric.name;
		} else {
			name = "nageru_" + metric.name + "{";
			bool first = true;
			for (const pair<string, string> &label : metric.labels) {
				if (!first) {
					name += ",";
				}
				first = false;
				name += label.first + "=\"" + label.second + "\"";
			}
			name += "}";
		}

		if (metric.data_type == DATA_TYPE_INT64) {
			ss << "nageru_" << name << " " << metric.location_int64->load() << "\n";
		} else {
			ss << "nageru_" << name << " " << metric.location_double->load() << "\n";
		}
	}

	return ss.str();
}
