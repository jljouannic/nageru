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
	void register_int_metric(const std::string &name, std::atomic<int64_t> *location);
	void register_double_metric(const std::string &name, std::atomic<double> *location);
	std::string serialize() const;

private:
	mutable std::mutex mu;
	std::unordered_map<std::string, std::atomic<int64_t> *> int_metrics;
	std::unordered_map<std::string, std::atomic<double> *> double_metrics;
};

extern Metrics global_metrics;

#endif  // !defined(_METRICS_H)
