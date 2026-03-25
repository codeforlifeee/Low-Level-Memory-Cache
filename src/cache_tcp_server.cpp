#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include "mini_redis/sharded_cache.hpp"
#include "mini_redis/tcp_command_parser.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace {

using Cache = mini_redis::ShardedCache<std::string, std::string, 64>;

void close_socket(SocketHandle s) {
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

bool send_all(SocketHandle client, const std::string& data) {
    std::size_t sent_total = 0;
    while (sent_total < data.size()) {
        const char* ptr = data.data() + sent_total;
        const int to_send = static_cast<int>(data.size() - sent_total);
#ifdef _WIN32
        const int sent = send(client, ptr, to_send, 0);
#else
        const int sent = static_cast<int>(send(client, ptr, static_cast<std::size_t>(to_send), 0));
#endif
        if (sent <= 0) {
            return false;
        }
        sent_total += static_cast<std::size_t>(sent);
    }
    return true;
}

std::string sanitize_line(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());

    for (unsigned char c : raw) {
        if (c == '\0') {
            continue;
        }
        if (c == '\r') {
            continue;
        }
        // Keep printable characters and common whitespace; drop other control bytes.
        if ((c >= 32 && c <= 126) || c == '\t' || c == ' ') {
            out.push_back(static_cast<char>(c));
        }
    }

    return mini_redis::tcp_trim(out);
}

std::string execute_command(const mini_redis::ParsedTcpCommand& cmd, Cache& cache, bool& should_quit) {
    using namespace std::chrono_literals;

    should_quit = false;

    switch (cmd.type) {
        case mini_redis::TcpCommandType::Ping:
            return "OK";
        case mini_redis::TcpCommandType::Help:
            return "OK commands: SET GET DEL STATS PING QUIT";
        case mini_redis::TcpCommandType::Quit:
            should_quit = true;
            return "OK";
        case mini_redis::TcpCommandType::Set: {
            std::optional<std::chrono::milliseconds> ttl = std::nullopt;
            if (cmd.ttl_ms.has_value()) {
                ttl = std::chrono::milliseconds(cmd.ttl_ms.value());
            }
            const bool ok = cache.set(cmd.key, cmd.value, ttl);
            return ok ? "OK" : "ERROR set failed";
        }
        case mini_redis::TcpCommandType::Get: {
            const auto value = cache.get(cmd.key);
            if (!value.has_value()) {
                return "(nil)";
            }
            return *value;
        }
        case mini_redis::TcpCommandType::Del: {
            const bool removed = cache.del(cmd.key);
            return removed ? "OK" : "(nil)";
        }
        case mini_redis::TcpCommandType::Stats: {
            const auto s = cache.stats();
            std::ostringstream oss;
            oss << "OK hits=" << s.hits
                << " misses=" << s.misses
                << " hit_ratio=" << s.hit_ratio()
                << " evictions=" << s.evictions
                << " expirations=" << s.expirations
                << " active_keys=" << s.active_keys
                << " memory_bytes=" << s.memory_bytes;
            return oss.str();
        }
        case mini_redis::TcpCommandType::Invalid:
        default:
            return "ERROR invalid command";
    }
}

void handle_client(SocketHandle client, Cache& cache) {
    std::string buffer;
    buffer.reserve(2048);

    const std::string greeting = "OK mini-redis tcp server ready\n";
    if (!send_all(client, greeting)) {
        close_socket(client);
        return;
    }

    char chunk[1024];
    bool alive = true;

    while (alive) {
#ifdef _WIN32
        const int n = recv(client, chunk, static_cast<int>(sizeof(chunk)), 0);
#else
        const int n = static_cast<int>(recv(client, chunk, sizeof(chunk), 0));
#endif
        if (n <= 0) {
            break;
        }

        buffer.append(chunk, static_cast<std::size_t>(n));

        std::size_t newline_pos = std::string::npos;
        while ((newline_pos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newline_pos);
            buffer.erase(0, newline_pos + 1);

            line = sanitize_line(line);
            if (line.empty()) {
                // Ignore empty lines to keep client protocol interaction clean.
                continue;
            }

            const auto parsed = mini_redis::parse_tcp_command(line);
            bool should_quit = false;
            std::string response;
            if (parsed.type == mini_redis::TcpCommandType::Invalid) {
                if (parsed.error == "empty command") {
                    continue;
                }
                response = "ERROR " + parsed.error;
            } else {
                response = execute_command(parsed, cache, should_quit);
            }

            response.push_back('\n');
            if (!send_all(client, response)) {
                alive = false;
                break;
            }

            if (should_quit) {
                alive = false;
                break;
            }
        }
    }

    close_socket(client);
}

bool init_sockets() {
#ifdef _WIN32
    WSADATA wsa_data;
    const int rc = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    return rc == 0;
#else
    return true;
#endif
}

void shutdown_sockets() {
#ifdef _WIN32
    WSACleanup();
#endif
}

}  // namespace

int main(int argc, char** argv) {
    int port = 6379;
    std::size_t capacity = 50000;

    if (argc > 1) {
        try {
            port = std::stoi(argv[1]);
        } catch (...) {
            std::cerr << "Invalid port. Usage: cache_tcp_server [port] [capacity]\n";
            return 1;
        }
    }
    if (argc > 2) {
        try {
            capacity = static_cast<std::size_t>(std::stoull(argv[2]));
        } catch (...) {
            std::cerr << "Invalid capacity. Usage: cache_tcp_server [port] [capacity]\n";
            return 1;
        }
    }

    if (port <= 0 || port > 65535) {
        std::cerr << "Port must be in range 1..65535\n";
        return 1;
    }

    if (!init_sockets()) {
        std::cerr << "Failed to initialize sockets\n";
        return 1;
    }

    Cache cache(capacity, true);

    SocketHandle server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == kInvalidSocket) {
        std::cerr << "Failed to create server socket\n";
        shutdown_sockets();
        return 1;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::cerr << "Bind failed on port " << port << "\n";
        close_socket(server);
        shutdown_sockets();
        return 1;
    }

    if (listen(server, 64) < 0) {
        std::cerr << "Listen failed\n";
        close_socket(server);
        shutdown_sockets();
        return 1;
    }

    std::cout << "Mini Redis TCP server listening on port " << port << "\n";
    std::cout << "Supported commands: SET key value [ttl_ms], GET key, DEL key, STATS, PING, QUIT\n";

    while (true) {
        sockaddr_in client_addr{};
#ifdef _WIN32
        int client_len = sizeof(client_addr);
#else
        socklen_t client_len = sizeof(client_addr);
#endif

        SocketHandle client = accept(server, reinterpret_cast<sockaddr*>(&client_addr), &client_len);
        if (client == kInvalidSocket) {
            continue;
        }

        std::thread([client, &cache] { handle_client(client, cache); }).detach();
    }

    close_socket(server);
    shutdown_sockets();
    return 0;
}
