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

    static std::thread& start_on_thread(AgentRunner& runner) {
        runner.thread_ = std::thread([&runner]() {
            runner.run();
        });
        return runner.thread_;
    }

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

    bool should_stop() const {
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
    std::thread thread_;
};

}
