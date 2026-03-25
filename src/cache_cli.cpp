#include <iostream>
#include <string>

#include "mini_redis/command_processor.hpp"

int main(int argc, char** argv) {
    std::size_t capacity = 50000;
    if (argc > 1) {
        try {
            capacity = static_cast<std::size_t>(std::stoull(argv[1]));
        } catch (...) {
            std::cerr << "Invalid capacity. Usage: cache_cli [capacity]\n";
            return 1;
        }
    }

    mini_redis::RedisCommandProcessor processor(capacity);

    std::cout << "Mini Redis CLI started. Type HELP for commands.\n";

    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) {
            break;
        }

        try {
            const auto response = processor.execute_line(line);
            std::cout << response.payload << "\n";
            if (response.should_exit) {
                break;
            }
        } catch (const std::exception& ex) {
            std::cout << "ERR " << ex.what() << "\n";
        }
    }

    return 0;
}
