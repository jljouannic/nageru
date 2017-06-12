#ifndef _METRICS_H
#define _METRICS_H 1

// A simple global class to keep track of metrics export in Prometheus format.
// It would be better to use a more full-featured Prometheus client library for this,
// but it would introduce a dependency that is not commonly packaged in distributions,
// which makes it quite unwieldy. Thus, we'll package our own for the time being.

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class Histogram;

// Prometheus recommends the use of timestamps instead of “time since event”,
// so you can use this to get the number of seconds since the epoch.
// Note that this will be wrong if your clock changes, so for non-metric use,
// you should use std::chrono::steady_clock instead.
double get_timestamp_for_metrics();

class Metrics {
public:
	enum Type {
		TYPE_COUNTER,
		TYPE_GAUGE,
		TYPE_HISTOGRAM,  // Internal use only.
	};

	void add(const std::string &name, std::atomic<int64_t> *location, Type type = TYPE_COUNTER)
	{
		add(name, {}, location, type);
	}

	void add(const std::string &name, std::atomic<double> *location, Type type = TYPE_COUNTER)
	{
		add(name, {}, location, type);
	}

	void add(const std::string &name, Histogram *location)
	{
		add(name, {}, location);
	}

	void add(const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels, std::atomic<int64_t> *location, Type type = TYPE_COUNTER);
	void add(const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels, std::atomic<double> *location, Type type = TYPE_COUNTER);
	void add(const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels, Histogram *location);

	std::string serialize() const;

private:
	enum DataType {
		DATA_TYPE_INT64,
		DATA_TYPE_DOUBLE,
		DATA_TYPE_HISTOGRAM,
	};

	struct Metric {
		DataType data_type;
		std::string name;
		std::vector<std::pair<std::string, std::string>> labels;
		union {
			std::atomic<int64_t> *location_int64;
			std::atomic<double> *location_double;
			Histogram *location_histogram;
		};
	};

	mutable std::mutex mu;
	std::map<std::string, Type> types;
	std::vector<Metric> metrics;
	std::vector<Histogram> histograms;
};

class Histogram {
public:
	void init(const std::vector<double> &bucket_vals);
	void init_uniform(size_t num_buckets);  // Sets up buckets 0..(N-1).
	void init_geometric(double min, double max, size_t num_buckets);
	void count_event(double val);
	std::string serialize(const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels) const;

private:
	// Bucket <i> counts number of events where val[i - 1] < x <= val[i].
	// The end histogram ends up being made into a cumulative one,
	// but that's not how we store it here.
	struct Bucket {
		double val;
		std::atomic<int64_t> count{0};
	};
	std::unique_ptr<Bucket[]> buckets;
	size_t num_buckets;
	std::atomic<double> sum{0.0};
	std::atomic<int64_t> count_after_last_bucket{0};
};

extern Metrics global_metrics;

#endif  // !defined(_METRICS_H)
