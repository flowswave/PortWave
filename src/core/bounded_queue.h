#pragma once
#include <algorithm>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>

namespace portwave {
// Shared worker/GUI byte queue. Drops oldest bytes with an observable loss counter.
class BoundedQueue {
  public:
    explicit BoundedQueue(std::size_t capacity = 1024 * 1024) : capacity_(capacity) {}
    void push(std::string bytes) {
        std::lock_guard lock(mutex_);
        if (bytes.size() > capacity_) {
            dropped_ += bytes.size() - capacity_;
            bytes.erase(0, bytes.size() - capacity_);
        }
        while (size_ + bytes.size() > capacity_ && !queue_.empty()) {
            dropped_ += queue_.front().size();
            size_ -= queue_.front().size();
            queue_.pop_front();
        }
        size_ += bytes.size();
        if (!bytes.empty())
            queue_.push_back(std::move(bytes));
    }
    std::string take(std::size_t budget) {
        std::lock_guard lock(mutex_);
        std::string out;
        out.reserve(std::min(budget, size_));
        while (!queue_.empty() && out.size() < budget) {
            auto& front = queue_.front();
            auto n = std::min(budget - out.size(), front.size());
            out.append(front, 0, n);
            front.erase(0, n);
            size_ -= n;
            if (front.empty())
                queue_.pop_front();
        }
        return out;
    }
    // Atomically discard older display backlog and return at most budget bytes.
    // skipped reports deliberate discard; dropped() includes it. Thread-safe;
    // independent consumers such as the disk logger are not affected.
    std::string takeLatest(std::size_t budget, bool& skipped) {
        std::lock_guard lock(mutex_);
        std::size_t discard = size_ > budget ? size_ - budget : 0;
        skipped = discard != 0;
        dropped_ += discard;
        while (discard && !queue_.empty()) {
            auto& front = queue_.front();
            const auto count = std::min(discard, front.size());
            front.erase(0, count);
            size_ -= count;
            discard -= count;
            if (front.empty())
                queue_.pop_front();
        }
        std::string out;
        out.reserve(size_);
        for (const auto& chunk : queue_)
            out += chunk;
        queue_.clear();
        size_ = 0;
        return out;
    }
    std::size_t dropped() const {
        std::lock_guard lock(mutex_);
        return dropped_;
    }
    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return size_;
    }

  private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<std::string> queue_;
    std::size_t size_{}, dropped_{};
};
} // namespace portwave
