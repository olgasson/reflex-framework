#pragma once

#include <thread>
#include <atomic>
#include <memory>
#include <functional>
#include <exception>
#include <stdexcept>
#include <iostream>
#include "agent.hpp"
#include "idle_strategy.hpp"

namespace reflex {

// Forward declarations
class AgentTerminationException : public std::exception {
public:
    explicit AgentTerminationException(const std::string& message) : message_(message) {}
    const char* what() const noexcept override { return message_.c_str(); }
private:
    std::string message_;
};

using ErrorHandler = std::function<void(const std::exception&)>;

class AgentRunner {
public:
    AgentRunner(IdleStrategy& idle_strategy, ErrorHandler error_handler, Agent& agent)
        : idle_strategy_(idle_strategy)
        , error_handler_(error_handler ? error_handler : default_error_handler)
        , agent_(agent)
        , is_running_(true)
        , is_closed_(false)
        , thread_() {
    }

    ~AgentRunner() {
        if (!is_closed_) {
            close();
        }
    }

    // Start the agent on a new thread that the runner owns. Returns a reference
    // to the owned thread; the caller must NOT join it — close() does that.
    // (The previous version stored the address of a by-value-returned local,
    // which dangled the instant the function returned.)
    static std::thread& start_on_thread(AgentRunner& runner) {
        runner.thread_ = std::thread([&runner]() {
            runner.run();
        });
        return runner.thread_;
    }

    // Static method with custom thread creation
    template<typename ThreadFactory>
    static std::thread& start_on_thread(AgentRunner& runner, ThreadFactory&& factory) {
        runner.thread_ = factory([&runner]() {
            runner.run();
        });
        return runner.thread_;
    }

    Agent& agent() {
        return agent_;
    }

    bool is_closed() const {
        return is_closed_;
    }

    std::thread& thread() {
        return thread_;
    }

    // Thread body: exceptions must never escape here — this runs as the raw
    // std::thread entry point, and an escaping exception would call
    // std::terminate. All failures are routed to error_handler_ instead.
    void run() {
        try {
            agent_.on_start();
            work_loop();
            agent_.on_close();
        } catch (const std::exception& e) {
            is_running_ = false;
            error_handler_(e);
        } catch (...) {
            is_running_ = false;
            error_handler_(std::runtime_error("Unknown exception in agent thread"));
        }

        is_closed_ = true;
    }

    void close() {
        is_running_ = false;

        if (thread_.joinable()) {
            try {
                thread_.join();
            } catch (const std::exception& e) {
                error_handler_(e);
            }
        }

        if (!is_closed_) {
            try {
                agent_.on_close();
            } catch (const std::exception& e) {
                error_handler_(e);
            }
        }

        is_closed_ = true;
    }

private:
    void work_loop() {
        while (is_running_) {
            do_work();
        }
    }

    void do_work() {
        try {
            int work_count = agent_.do_work();
            idle_strategy_.idle(work_count);
            
            // Check for thread interruption equivalent
            if (work_count <= 0 && should_stop()) {
                is_running_ = false;
            }
        } catch (const AgentTerminationException& ex) {
            is_running_ = false;
            handle_error(ex);
        } catch (const std::exception& e) {
            if (should_stop()) {
                is_running_ = false;
            }
            
            handle_error(e);
            
            if (is_running_ && should_stop()) {
                is_running_ = false;
            }
        }
    }

    void handle_error(const std::exception& e) {
        if (is_running_) {
            error_handler_(e);
        }
    }

    // Simple check for stopping condition - can be enhanced
    bool should_stop() const {
        // In a more complete implementation, this could check for 
        // thread interruption signals or other stop conditions
        return false;
    }

    static void default_error_handler(const std::exception& e) {
        std::cerr << "Agent error: " << e.what() << std::endl;
    }

    IdleStrategy& idle_strategy_;
    ErrorHandler error_handler_;
    Agent& agent_;
    std::atomic<bool> is_running_;
    std::atomic<bool> is_closed_;
    std::thread thread_; // Owned; default-constructed (empty) unless started via start_on_thread()
};

} // namespace reflex
