#include "metrics.h"

#include <assert.h>
#include <math.h>

#include <algorithm>
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

void Metrics::add(const string &name, const vector<pair<string, string>> &labels, Histogram *location)
{
	Metric metric;
	metric.data_type = DATA_TYPE_HISTOGRAM;
	metric.name = name;
	metric.labels = labels;
	metric.location_histogram = location;

	lock_guard<mutex> lock(mu);
	metrics.push_back(metric);
	assert(types.count(name) == 0 || types[name] == TYPE_HISTOGRAM);
	types[name] = TYPE_HISTOGRAM;
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
		} else if (name_and_type.second == TYPE_HISTOGRAM) {
			ss << "# TYPE nageru_" << name_and_type.first << " histogram\n";
		}
	}
	for (const Metric &metric : metrics) {
		string name = serialize_name(metric.name, metric.labels);

		if (metric.data_type == DATA_TYPE_INT64) {
			ss << name << " " << metric.location_int64->load() << "\n";
		} else if (metric.data_type == DATA_TYPE_DOUBLE) {
			ss << name << " " << metric.location_double->load() << "\n";
		} else {
			ss << metric.location_histogram->serialize(metric.name, metric.labels);
		}
	}

	return ss.str();
}

void Histogram::init(const vector<double> &bucket_vals)
{
	this->num_buckets = bucket_vals.size();
	buckets.reset(new Bucket[num_buckets]);
	for (size_t i = 0; i < num_buckets; ++i) {
		buckets[i].val = bucket_vals[i];
	}
}

void Histogram::init_uniform(size_t num_buckets)
{
	this->num_buckets = num_buckets;
	buckets.reset(new Bucket[num_buckets]);
	for (size_t i = 0; i < num_buckets; ++i) {
		buckets[i].val = i;
	}
}

void Histogram::init_geometric(double min, double max, size_t num_buckets)
{
	this->num_buckets = num_buckets;
	buckets.reset(new Bucket[num_buckets]);
	for (size_t i = 0; i < num_buckets; ++i) {
		buckets[i].val = min * pow(max / min, double(i) / (num_buckets - 1));
	}
}

void Histogram::count_event(double val)
{
	Bucket ref_bucket;
	ref_bucket.val = val;
	auto it = lower_bound(buckets.get(), buckets.get() + num_buckets, ref_bucket,
		[](const Bucket &a, const Bucket &b) { return a.val < b.val; });
	if (it == buckets.get() + num_buckets) {
		++count_after_last_bucket;
	} else {
		++it->count;
	}
	// Non-atomic add, but that's fine, since there are no concurrent writers.
	sum = sum + val;
}

string Histogram::serialize(const string &name, const vector<pair<string, string>> &labels) const
{
	stringstream ss;
	ss.imbue(locale("C"));
	ss.precision(20);

	int64_t count = 0;
	for (size_t bucket_idx = 0; bucket_idx < num_buckets; ++bucket_idx) {
		stringstream le_ss;
		le_ss.imbue(locale("C"));
		le_ss.precision(20);
		le_ss << buckets[bucket_idx].val;
		vector<pair<string, string>> bucket_labels = labels;
		bucket_labels.emplace_back("le", le_ss.str());

		count += buckets[bucket_idx].count.load();
		ss << serialize_name(name + "_bucket", bucket_labels) << " " << count << "\n";
	}

	count += count_after_last_bucket.load();

	ss << serialize_name(name + "_sum", labels) << " " << sum.load() << "\n";
	ss << serialize_name(name + "_count", labels) << " " << count << "\n";

	return ss.str();
}
