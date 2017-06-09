#ifndef _METRICS_H
#define _METRICS_H 1

// A simple global class to keep track of metrics export in Prometheus format.
// It would be better to use a more full-featured Prometheus client library for this,
// but it would introduce a dependency that is not commonly packaged in distributions,
// which makes it quite unwieldy. Thus, we'll package our own for the time being.

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>

class Metrics {
public:
	enum Type {
		TYPE_COUNTER,
		TYPE_GAUGE,
	};

	void register_int_metric(const std::string &name, std::atomic<int64_t> *location, Type type = TYPE_COUNTER);
	void register_double_metric(const std::string &name, std::atomic<double> *location, Type type = TYPE_COUNTER);
	std::string serialize() const;

private:
	template<class T>
	struct Metric {
		Type type;
		std::atomic<T> *location;
	};

	mutable std::mutex mu;
	std::unordered_map<std::string, Metric<int64_t>> int_metrics;
	std::unordered_map<std::string, Metric<double>> double_metrics;
};

extern Metrics global_metrics;

#endif  // !defined(_METRICS_H)
