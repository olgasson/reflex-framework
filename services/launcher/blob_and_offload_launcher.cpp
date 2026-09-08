#include <iostream>
#include <memory>
#include <thread>

#include "launcher_common.hpp"

#include "../../include/framework/agent_runner.hpp"

#include "../../include/components/nio/nio_component.hpp"
#include "../../include/components/blob/okx_blob.hpp"
#include "../../include/components/data_offload/logger_component.hpp"

#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/spin_wait_strategy.hpp>
#include <disruptorplus/sequence_barrier.hpp>

#include "disruptorplus/single_threaded_claim_strategy.hpp"

int main() {
  try {
    launcher::install_signal_handlers();

    reflex::ComponentConfig logger_config{
      .component_name_ = "logger",
      .environment_ = "test",
    };

    auto log_buffer = std::make_shared<disruptorplus::ring_buffer<reflex::MessageSlot>>(launcher::kRingBufferSize);

    auto wait_strategy = std::make_shared<disruptorplus::spin_wait_strategy>();

    auto claim_strategy = std::make_shared<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>>(
        launcher::kRingBufferSize, *wait_strategy);

    auto logger_barrier = std::make_shared<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>>(*wait_strategy);

    claim_strategy->add_claim_barrier(*logger_barrier);

    auto nio_component = std::make_unique<reflex::NIOComponent>(launcher::make_nio_config());
    auto nio_idle_strategy = launcher::make_idle_strategy();

    auto blob_component = std::make_unique<reflex::OkxBlob>(launcher::make_blob_config(), log_buffer, claim_strategy);
    nio_component->add_component(std::move(blob_component));

    auto logger_component = std::make_unique<reflex::LoggerComponent>(logger_config, log_buffer, logger_barrier, claim_strategy);
    auto logger_idle_strategy = launcher::make_idle_strategy();

    auto error_handler = [](const std::exception& e) {
      std::cerr << "Agent error: " << e.what() << std::endl;
    };

    reflex::AgentRunner nio_runner(*nio_idle_strategy, error_handler, *nio_component);
    reflex::AgentRunner logger_runner(*logger_idle_strategy, error_handler, *logger_component);

    std::thread nio_thread([&nio_runner]() {
        nio_runner.run();
    });

    std::thread logger_thread([&logger_runner]() {
        logger_runner.run();
    });

    std::cout << "Started agents with blob and logger components on separate threads" << std::endl;

    launcher::await_shutdown();

    std::cout << "Closing agents..." << std::endl;

    nio_runner.close();
    logger_runner.close();

    if (nio_thread.joinable()) {
      nio_thread.join();
    }
    if (logger_thread.joinable()) {
      logger_thread.join();
    }

    std::cout << "All agents closed successfully" << std::endl;

  } catch (const std::exception& e) {
    std::cerr << "Application error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
