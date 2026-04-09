#include <chrono>
#include <ctime>
#include <iostream>
#include <string>

int main() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    std::cout << "[MiMoCA] Startup at " << std::ctime(&now_time);
    std::cout << "[MiMoCA] Windows-first prototype shell is running.\n";
    std::cout << "[MiMoCA] No model integrations are enabled yet.\n";
    std::cout << "[MiMoCA] Press Enter to exit...\n";

    std::string line;
    std::getline(std::cin, line);
    return 0;
}
