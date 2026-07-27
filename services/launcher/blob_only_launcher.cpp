#include <iostream>
#include <memory>
#include <thread>

#include "launcher_common.hpp"

#include "../../include/framework/agent_runner.hpp"
#include "../../include/framework/noop_idle_strategy.hpp"

#include "../../include/components/nio/nio_component.hpp"
#include "../../include/components/blob/okx_blob.hpp"

int main() {
  try {
    launcher::install_signal_handlers();

    // Set up DisruptorPlus SPMC buffer
    auto log_buffer = std::make_shared<disruptorplus::ring_buffer<reflex::MessageSlot>>(launcher::kRingBufferSize);

    // Create wait strategy (spin wait for low latency)
    auto wait_strategy = std::make_shared<disruptorplus::spin_wait_strategy>();

    // Create claim strategy for single producer (NIO component)
    auto claim_strategy = std::make_shared<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>>(
        launcher::kRingBufferSize, *wait_strategy);

    auto nio_component = std::make_unique<reflex::NIOComponent>(launcher::make_nio_config());
    auto nio_idle_strategy = std::make_unique<reflex::NoopIdleStrategy>();

    // Add your blob to the NIO component
    auto blob_component = std::make_unique<reflex::OkxBlob>(launcher::make_blob_config(), log_buffer, claim_strategy);
    nio_component->add_component(std::move(blob_component));

    // Error handler
    auto error_handler = [](const std::exception& e) {
      std::cerr << "Agent error: " << e.what() << std::endl;
    };

    // Create service runner with nio component
    reflex::AgentRunner nio_runner(*nio_idle_strategy, error_handler, *nio_component);

    // Start the runner on a thread
    std::thread nio_thread([&nio_runner]() {
        nio_runner.run();
    });

    std::cout << "Started agents on threads" << std::endl;

    launcher::await_shutdown();

    std::cout << "Closing agents..." << std::endl;

    // Stop the service runner
    nio_runner.close();

    // Wait for threads to complete
    if (nio_thread.joinable()) {
      nio_thread.join();
    }

    std::cout << "All agents closed successfully" << std::endl;

  } catch (const std::exception& e) {
    std::cerr << "Application error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
