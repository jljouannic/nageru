#ifndef _QUITTABLE_SLEEPER
#define _QUITTABLE_SLEEPER 1

// A class that assists with fast shutdown of threads. You can set
// a flag that says the thread should quit, which it can then check
// in a loop -- and if the thread sleeps (using the sleep_* functions
// on the class), that sleep will immediately be aborted.
//
// All member functions on this class are thread-safe.

#include <chrono>
#include <mutex>

class QuittableSleeper {
public:
	void quit()
	{
		std::lock_guard<std::mutex> l(mu);
		should_quit_var = true;
		quit_cond.notify_all();
	}

	void unquit()
	{
		std::lock_guard<std::mutex> l(mu);
		should_quit_var = false;
	}

	bool should_quit() const
	{
		std::lock_guard<std::mutex> l(mu);
		return should_quit_var;
	}

	template<class Rep, class Period>
	void sleep_for(const std::chrono::duration<Rep, Period> &duration)
	{
		std::chrono::steady_clock::time_point t =
			std::chrono::steady_clock::now() +
			std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);
		sleep_until(t);
	}

	template<class Clock, class Duration>
	void sleep_until(const std::chrono::time_point<Clock, Duration> &t)
	{
		std::unique_lock<std::mutex> lock(mu);
		quit_cond.wait_until(lock, t, [this]{
			return should_quit_var;
		});
	}

private:
	mutable std::mutex mu;
	bool should_quit_var = false;
	std::condition_variable quit_cond;
};

#endif  // !defined(_QUITTABLE_SLEEPER) 
