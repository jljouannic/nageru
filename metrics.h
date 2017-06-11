#ifndef _METRICS_H
#define _METRICS_H 1

// A simple global class to keep track of metrics export in Prometheus format.
// It would be better to use a more full-featured Prometheus client library for this,
// but it would introduce a dependency that is not commonly packaged in distributions,
// which makes it quite unwieldy. Thus, we'll package our own for the time being.

#include <atomic>
#include <mutex>
#include <string>
#include <map>
#include <vector>

class Metrics {
public:
	enum Type {
		TYPE_COUNTER,
		TYPE_GAUGE,
	};

	void add(const std::string &name, std::atomic<int64_t> *location, Type type = TYPE_COUNTER)
	{
		add(name, {}, location, type);
	}

	void add(const std::string &name, std::atomic<double> *location, Type type = TYPE_COUNTER)
	{
		add(name, {}, location, type);
	}

	void add(const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels, std::atomic<int64_t> *location, Type type = TYPE_COUNTER);
	void add(const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels, std::atomic<double> *location, Type type = TYPE_COUNTER);

	// Only integer histogram, ie. keys are 0..(N-1).
	void add_histogram(const std::string &name, const std::vector<std::pair<std::string, std::string>> &labels, std::atomic<int64_t> *location, size_t num_elements);

	std::string serialize() const;

private:
	enum DataType {
		DATA_TYPE_INT64,
		DATA_TYPE_DOUBLE,
	};

	struct Metric {
		DataType data_type;
		std::string name;
		std::vector<std::pair<std::string, std::string>> labels;
		union {
			std::atomic<int64_t> *location_int64;
			std::atomic<double> *location_double;
		};
	};

	// TODO: This needs to be more general.
	struct Histogram {
		std::string name;
		std::vector<std::pair<std::string, std::string>> labels;
		std::atomic<int64_t> *location_int64;  // First bucket.
		size_t num_elements;
	};

	mutable std::mutex mu;
	std::map<std::string, Type> types;
	std::vector<Metric> metrics;
	std::vector<Histogram> histograms;
};

extern Metrics global_metrics;

#endif  // !defined(_METRICS_H)
