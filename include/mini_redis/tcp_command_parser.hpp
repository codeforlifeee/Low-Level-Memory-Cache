#pragma once

#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace mini_redis {

enum class TcpCommandType {
    Set,
    Get,
    Del,
    Stats,
    Ping,
    Quit,
    Help,
    Invalid,
};

struct ParsedTcpCommand {
    TcpCommandType type{TcpCommandType::Invalid};
    std::string key;
    std::string value;
    std::optional<long long> ttl_ms;
    std::string error;
};

inline std::string tcp_trim(const std::string& input) {
    std::size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])) != 0) {
        ++start;
    }

    std::size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }

    return input.substr(start, end - start);
}

inline std::string tcp_upper(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

inline std::vector<std::string> tcp_split_ws(const std::string& input) {
    std::istringstream iss(input);
    std::vector<std::string> out;
    std::string token;
    while (iss >> token) {
        out.push_back(token);
    }
    return out;
}

inline ParsedTcpCommand parse_tcp_command(const std::string& raw_line) {
    ParsedTcpCommand out;

    const std::string line = tcp_trim(raw_line);
    if (line.empty()) {
        out.error = "empty command";
        return out;
    }

    const auto parts = tcp_split_ws(line);
    if (parts.empty()) {
        out.error = "empty command";
        return out;
    }

    const std::string cmd = tcp_upper(parts[0]);

    if (cmd == "PING") {
        if (parts.size() != 1) {
            out.error = "PING takes no arguments";
            return out;
        }
        out.type = TcpCommandType::Ping;
        return out;
    }

    if (cmd == "HELP") {
        out.type = TcpCommandType::Help;
        return out;
    }

    if (cmd == "QUIT" || cmd == "EXIT") {
        out.type = TcpCommandType::Quit;
        return out;
    }

    if (cmd == "STATS") {
        if (parts.size() != 1) {
            out.error = "STATS takes no arguments";
            return out;
        }
        out.type = TcpCommandType::Stats;
        return out;
    }

    if (cmd == "GET") {
        if (parts.size() != 2) {
            out.error = "usage: GET <key>";
            return out;
        }
        out.type = TcpCommandType::Get;
        out.key = parts[1];
        return out;
    }

    if (cmd == "DEL") {
        if (parts.size() != 2) {
            out.error = "usage: DEL <key>";
            return out;
        }
        out.type = TcpCommandType::Del;
        out.key = parts[1];
        return out;
    }

    if (cmd == "SET") {
        if (parts.size() < 3 || parts.size() > 4) {
            out.error = "usage: SET <key> <value> [ttl_ms]";
            return out;
        }

        out.type = TcpCommandType::Set;
        out.key = parts[1];
        out.value = parts[2];

        if (parts.size() == 4) {
            std::size_t pos = 0;
            long long ttl = 0;
            try {
                ttl = std::stoll(parts[3], &pos);
            } catch (...) {
                out.type = TcpCommandType::Invalid;
                out.error = "ttl_ms must be an integer";
                return out;
            }

            if (pos != parts[3].size() || ttl < 0) {
                out.type = TcpCommandType::Invalid;
                out.error = "ttl_ms must be a non-negative integer";
                return out;
            }
            out.ttl_ms = ttl;
        }

        return out;
    }

    out.error = "unknown command";
    return out;
}

}  // namespace mini_redis
