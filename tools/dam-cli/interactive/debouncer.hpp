#pragma once

#include <chrono>

namespace dam::cli {

/**
 * Simple debouncer for input handling.
 *
 * Ensures that a function is only called after a specified delay
 * has passed since the last trigger. This prevents excessive API calls
 * while the user is actively typing.
 *
 * Usage:
 *   Debouncer debouncer(450);  // 450ms delay
 *
 *   // On each keystroke:
 *   debouncer.trigger();
 *
 *   // In main loop:
 *   if (debouncer.ready()) {
 *       // Debounce period elapsed, safe to make API call
 *       request_completion();
 *   }
 */
class Debouncer {
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::milliseconds;
    using TimePoint = Clock::time_point;

    /**
     * Create a debouncer with the specified delay.
     * @param delay_ms Debounce delay in milliseconds
     */
    explicit Debouncer(int delay_ms)
        : delay_(delay_ms)
        , last_trigger_(Clock::now())
    {}

    /**
     * Trigger the debouncer. Resets the timer.
     * Call this on each user input event.
     */
    void trigger() {
        last_trigger_ = Clock::now();
        pending_ = true;
    }

    /**
     * Check if debounce period has elapsed and action is ready.
     * Clears the pending flag if ready (one-shot).
     * @return true if the delay has passed since last trigger
     */
    bool ready() {
        if (!pending_) {
            return false;
        }

        auto elapsed = Clock::now() - last_trigger_;
        if (elapsed >= delay_) {
            pending_ = false;
            return true;
        }
        return false;
    }

    /**
     * Check if ready without consuming the pending state.
     * Use this for polling without side effects.
     */
    bool peek_ready() const {
        if (!pending_) {
            return false;
        }
        auto elapsed = Clock::now() - last_trigger_;
        return elapsed >= delay_;
    }

    /**
     * Cancel any pending debounced action.
     * Use this when the user explicitly cancels or when
     * you want to reset state.
     */
    void cancel() {
        pending_ = false;
    }

    /**
     * Check if there's a pending action waiting for debounce.
     */
    bool is_pending() const {
        return pending_;
    }

    /**
     * Get remaining time until ready (in ms).
     * Returns 0 if not pending or already ready.
     */
    int remaining_ms() const {
        if (!pending_) {
            return 0;
        }

        auto elapsed = Clock::now() - last_trigger_;
        auto remaining = delay_ - std::chrono::duration_cast<Duration>(elapsed);

        if (remaining.count() <= 0) {
            return 0;
        }
        return static_cast<int>(remaining.count());
    }

    /**
     * Get the configured delay in milliseconds.
     */
    int delay_ms() const {
        return static_cast<int>(delay_.count());
    }

    /**
     * Set a new delay value.
     */
    void set_delay(int delay_ms) {
        delay_ = Duration(delay_ms);
    }

    /**
     * Get time since last trigger in milliseconds.
     */
    int elapsed_ms() const {
        auto elapsed = Clock::now() - last_trigger_;
        return static_cast<int>(
            std::chrono::duration_cast<Duration>(elapsed).count()
        );
    }

private:
    Duration delay_;
    TimePoint last_trigger_;
    bool pending_ = false;
};

}  // namespace dam::cli
