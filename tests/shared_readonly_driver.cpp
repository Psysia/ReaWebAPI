#include "core/shared_readonly.hpp"
#include <atomic>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace { bool hold = false; }
namespace reaweb {
void shared_read_test_hook() {
  if (!hold) return;
  hold = false;
  std::cout << "HANDLE_OPEN" << std::endl;
  std::string release; std::getline(std::cin, release);
  if (release != "release") throw std::runtime_error("test handshake failed");
}
}
int main() {
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream request(line);
    std::string mode, path, offset, count;
    if (!std::getline(request, mode, '\t') || !std::getline(request, path, '\t') ||
        !std::getline(request, offset, '\t') || !std::getline(request, count)) return 2;
    if (mode == "threads") {
      const auto expected = reaweb::shared_read_range_json(path.c_str(), "0", 65536);
      std::atomic<bool> ok{expected.find("\"ok\":true") != std::string::npos};
      std::vector<std::thread> workers;
      for (int worker = 0; worker < 8; ++worker) workers.emplace_back([&] {
        for (int index = 0; index < 25; ++index)
          if (reaweb::shared_read_range_json(path.c_str(), "0", 65536) != expected) ok = false;
      });
      for (auto& worker : workers) worker.join();
      std::cout << (ok ? "{\"ok\":true}" : "{\"ok\":false}") << std::endl;
    } else {
      hold = mode == "hold";
      std::cout << reaweb::shared_read_range_json(path.c_str(), offset.c_str(), std::stoi(count)) << std::endl;
    }
  }
}
