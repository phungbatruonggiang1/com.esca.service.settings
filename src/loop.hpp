// The service's one thread: the D-Bus connection, timers and posted work.
//
// Joining a Wi-Fi network or pairing a Bluetooth device means waiting for
// other daemons while still serving calls, with a deadline. sd-bus is not
// thread-safe, so instead of threads this loop polls the bus together with
// its own timers, and everything -- method handlers, signal handlers,
// timeouts -- runs here, one at a time.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include <sdbus-c++/sdbus-c++.h>

namespace esca::settings {

class Loop {
public:
    using Clock = std::chrono::steady_clock;
    using Task = std::function<void()>;

    explicit Loop(sdbus::IConnection &connection);
    ~Loop();
    Loop(const Loop &) = delete;
    Loop &operator=(const Loop &) = delete;

    /// Until quit().
    void run();
    /// Any thread, and from a signal handler.
    void quit();
    /// Any thread: runs `task` on the loop thread soon.
    void post(Task task);

    /// Loop thread: runs `task` after `delay`; returns an id for cancel().
    uint64_t after(std::chrono::milliseconds delay, Task task);
    void cancel(uint64_t id);

    /// Loop thread: runs `task` once, `delay` after the first of any number
    /// of calls with the same key -- for "something changed" bursts.
    void coalesce(const std::string &key, std::chrono::milliseconds delay, Task task);

private:
    void wake();
    void run_due_timers();

    sdbus::IConnection &connection_;
    int wake_fd_ = -1;
    std::atomic<bool> quit_ { false };

    std::mutex mutex_; ///< guards posted_
    std::deque<Task> posted_;

    uint64_t next_id_ = 1;
    std::multimap<Clock::time_point, std::pair<uint64_t, Task>> timers_;
    std::set<std::string> pending_keys_;
};

} // namespace esca::settings
