#include <chrono>
#include <ctime>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "Ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

void Log(const std::string& message) {
    std::cout << "[MiMoCA] " << message << '\n';
}

bool CheckPythonHealth(const std::string& host, int port) {
#ifdef _WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        Log("Failed to initialize Winsock.");
        return false;
    }
#endif

    int socket_fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, 0));
    if (socket_fd < 0) {
        Log("Failed to create socket for sidecar health check.");
#ifdef _WIN32
        WSACleanup();
#endif
        return false;
    }

    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
        Log("Invalid sidecar host address: " + host);
#ifdef _WIN32
        closesocket(socket_fd);
        WSACleanup();
#else
        close(socket_fd);
#endif
        return false;
    }

    if (::connect(socket_fd, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        Log("Could not connect to Python sidecar at http://" + host + ":" + std::to_string(port));
#ifdef _WIN32
        closesocket(socket_fd);
        WSACleanup();
#else
        close(socket_fd);
#endif
        return false;
    }

    const std::string request =
        "GET /health HTTP/1.1\r\n"
        "Host: " +
        host + "\r\n"
               "Connection: close\r\n\r\n";

#ifdef _WIN32
    const int sent = send(socket_fd, request.c_str(), static_cast<int>(request.size()), 0);
#else
    const ssize_t sent = send(socket_fd, request.c_str(), request.size(), 0);
#endif
    if (sent <= 0) {
        Log("Failed to send sidecar health request.");
#ifdef _WIN32
        closesocket(socket_fd);
        WSACleanup();
#else
        close(socket_fd);
#endif
        return false;
    }

    std::string response;
    char buffer[512];
    while (true) {
#ifdef _WIN32
        const int bytes_read = recv(socket_fd, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
        const ssize_t bytes_read = recv(socket_fd, buffer, sizeof(buffer), 0);
#endif
        if (bytes_read <= 0) {
            break;
        }
        response.append(buffer, static_cast<size_t>(bytes_read));
    }

#ifdef _WIN32
    closesocket(socket_fd);
    WSACleanup();
#else
    close(socket_fd);
#endif

    const bool ok = response.find("200 OK") != std::string::npos;
    if (ok) {
        Log("Python sidecar health check succeeded.");
    } else {
        Log("Python sidecar health check failed; unexpected response.");
    }

    Log("Sidecar response preview: " + response.substr(0, std::min<size_t>(response.size(), 180)));
    return ok;
}

}  // namespace

int main() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

    std::cout << "[MiMoCA] Startup at " << std::ctime(&now_time);
    Log("Windows-first prototype shell is running.");
    Log("Checking Python sidecar at http://127.0.0.1:8080/health");

    const bool sidecar_ok = CheckPythonHealth("127.0.0.1", 8080);
    if (!sidecar_ok) {
        Log("Continuing without Python sidecar (graceful degradation). Start python/service.py to enable it.");
    }

    Log("Press Enter to exit...");
    std::string line;
    std::getline(std::cin, line);
    return 0;
}
