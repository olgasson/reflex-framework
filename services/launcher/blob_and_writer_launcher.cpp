#include <iostream>
#include <memory>
#include <thread>

#include "launcher_common.hpp"

#include "../../include/framework/agent_runner.hpp"

#include "../../include/components/nio/nio_component.hpp"
#include "../../include/components/blob/okx_blob.hpp"
#include "../../include/components/data_offload/binary_writer_component.hpp"

#include <disruptorplus/ring_buffer.hpp>
#include <disruptorplus/spin_wait_strategy.hpp>
#include <disruptorplus/sequence_barrier.hpp>

#include "disruptorplus/single_threaded_claim_strategy.hpp"

int main() {
  try {
    launcher::install_signal_handlers();

    reflex::ComponentConfig binary_writer_base_config{
        .component_name_ = "binary_writer",
        .environment_ = "test",
    };

    const char* output_path_env = std::getenv("REFLEX_OUTPUT_PATH");
    const std::string output_path = output_path_env ? output_path_env : "./data";

    reflex::BinaryWriterConfig binary_writer_config(binary_writer_base_config, output_path.c_str());

    auto log_buffer = std::make_shared<disruptorplus::ring_buffer<reflex::MessageSlot>>(launcher::kRingBufferSize);

    auto wait_strategy = std::make_shared<disruptorplus::spin_wait_strategy>();

    auto claim_strategy =
        std::make_shared<disruptorplus::single_threaded_claim_strategy<disruptorplus::spin_wait_strategy>>(
            launcher::kRingBufferSize, *wait_strategy);

    auto binary_writer_barrier =
        std::make_shared<disruptorplus::sequence_barrier<disruptorplus::spin_wait_strategy>>(*wait_strategy);

    claim_strategy->add_claim_barrier(*binary_writer_barrier);

    auto nio_component = std::make_unique<reflex::NIOComponent>(launcher::make_nio_config());
    auto nio_idle_strategy = launcher::make_idle_strategy();

    auto blob_component = std::make_unique<reflex::OkxBlob>(launcher::make_blob_config(), log_buffer, claim_strategy);
    nio_component->add_component(std::move(blob_component));

    auto binary_writer_component = std::make_unique<reflex::BinaryWriterComponent>(
        binary_writer_config, log_buffer, binary_writer_barrier, claim_strategy);
    auto binary_writer_idle_strategy = launcher::make_idle_strategy();

    auto error_handler = [](const std::exception& e) { std::cerr << "Agent error: " << e.what() << std::endl; };

    reflex::AgentRunner nio_runner(*nio_idle_strategy, error_handler, *nio_component);
    reflex::AgentRunner binary_writer_runner(*binary_writer_idle_strategy, error_handler, *binary_writer_component);

    std::thread nio_thread([&nio_runner]() { nio_runner.run(); });

    std::thread binary_writer_thread([&binary_writer_runner]() { binary_writer_runner.run(); });

    std::cout << "Started agents with blob and binary writer components on separate threads" << std::endl;

    launcher::await_shutdown();

    std::cout << "Closing agents..." << std::endl;

    nio_runner.close();
    binary_writer_runner.close();

    if (nio_thread.joinable()) {
      nio_thread.join();
    }
    if (binary_writer_thread.joinable()) {
      binary_writer_thread.join();
    }

    std::cout << "All agents closed successfully" << std::endl;

  } catch (const std::exception& e) {
    std::cerr << "Application error: " << e.what() << std::endl;
    return 1;
  }

  return 0;
}
