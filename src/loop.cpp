#include "loop.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "logger.hpp"

namespace esca::settings {

Loop::Loop(sdbus::IConnection &connection) : connection_(connection)
{
    wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd_ < 0)
        throw std::runtime_error(std::string("eventfd: ") + std::strerror(errno));
}

Loop::~Loop()
{
    if (wake_fd_ >= 0)
        close(wake_fd_);
}

void Loop::wake()
{
    const uint64_t one = 1;
    (void)!write(wake_fd_, &one, sizeof(one));
}

void Loop::quit()
{
    quit_ = true;
    wake();
}

void Loop::post(Task task)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        posted_.push_back(std::move(task));
    }
    wake();
}

uint64_t Loop::after(std::chrono::milliseconds delay, Task task)
{
    const uint64_t id = next_id_++;
    timers_.emplace(Clock::now() + delay, std::make_pair(id, std::move(task)));
    return id;
}

void Loop::cancel(uint64_t id)
{
    for (auto it = timers_.begin(); it != timers_.end(); ++it)
        if (it->second.first == id) {
            timers_.erase(it);
            return;
        }
}

void Loop::coalesce(const std::string &key, std::chrono::milliseconds delay, Task task)
{
    if (!pending_keys_.insert(key).second)
        return;
    after(delay, [this, key, task = std::move(task)] {
        pending_keys_.erase(key);
        task();
    });
}

void Loop::run_due_timers()
{
    const auto now = Clock::now();
    while (!timers_.empty() && timers_.begin()->first <= now) {
        Task task = std::move(timers_.begin()->second.second);
        timers_.erase(timers_.begin());
        try {
            task();
        } catch (const std::exception &e) {
            ESCA_LOG_ERROR("timer task failed: %s", e.what());
        }
    }
}

void Loop::run()
{
    while (!quit_) {
        const sdbus::IConnection::PollData bus = connection_.getEventLoopPollData();
        int timeout = bus.getPollTimeout(); // -1: none
        if (!timers_.empty()) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timers_.begin()->first - Clock::now()).count();
            const int t = ms < 0 ? 0 : static_cast<int>(std::min<long long>(ms + 1, 60000));
            timeout = timeout < 0 ? t : std::min(timeout, t);
        }
        pollfd fds[3] = { { bus.fd, bus.events, 0 }, { bus.eventFd, POLLIN, 0 }, { wake_fd_, POLLIN, 0 } };
        if (poll(fds, 3, timeout) < 0 && errno != EINTR)
            throw std::runtime_error(std::string("poll: ") + std::strerror(errno));

        if (fds[2].revents & POLLIN) {
            uint64_t drained;
            (void)!read(wake_fd_, &drained, sizeof(drained));
        }
        // Everything the bus has, including what the handlers below send.
        while (connection_.processPendingEvent()) {
        }

        std::deque<Task> tasks;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks.swap(posted_);
        }
        for (Task &t : tasks) {
            try {
                t();
            } catch (const std::exception &e) {
                ESCA_LOG_ERROR("posted task failed: %s", e.what());
            }
        }
        run_due_timers();
    }
}

} // namespace esca::settings
