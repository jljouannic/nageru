#include "metrics.h"

#include <assert.h>

#include <locale>
#include <sstream>

using namespace std;

Metrics global_metrics;

namespace {

string serialize_name(const string &name, const vector<pair<string, string>> &labels)
{
	if (labels.empty()) {
		return "nageru_" + name;
	}

	string label_str;
	for (const pair<string, string> &label : labels) {
		if (!label_str.empty()) {
			label_str += ",";
		}
		label_str += label.first + "=\"" + label.second + "\"";
	}
	return "nageru_" + name + "{" + label_str + "}";
}

}  // namespace

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

void Metrics::add_histogram(const string &name, const vector<pair<string, string>> &labels, atomic<int64_t> *first_bucket_location, atomic<double> *sum_location, size_t num_elements)
{
	Histogram histogram;
	histogram.name = name;
	histogram.labels = labels;
	histogram.first_bucket_location = first_bucket_location;
	histogram.sum_location = sum_location;
	histogram.num_elements = num_elements;

	lock_guard<mutex> lock(mu);
	histograms.push_back(histogram);
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
		string name = serialize_name(metric.name, metric.labels);

		if (metric.data_type == DATA_TYPE_INT64) {
			ss << name << " " << metric.location_int64->load() << "\n";
		} else {
			ss << name << " " << metric.location_double->load() << "\n";
		}
	}
	for (const Histogram &histogram : histograms) {
		ss << "# TYPE nageru_" << histogram.name << " histogram\n";

		int64_t count = 0;
		for (size_t i = 0; i < histogram.num_elements; ++i) {
			char buf[16];
			snprintf(buf, sizeof(buf), "%lu", i);
			vector<pair<string, string>> labels = histogram.labels;
			labels.emplace_back("le", buf);

			int64_t val = histogram.first_bucket_location[i].load();
			count += val;
			ss << serialize_name(histogram.name + "_bucket", labels) << " " << count << "\n";
		}

		ss << serialize_name(histogram.name + "_sum", histogram.labels) << " " << histogram.sum_location->load() << "\n";
		ss << serialize_name(histogram.name + "_count", histogram.labels) << " " << count << "\n";
	}

	return ss.str();
}
