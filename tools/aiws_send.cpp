#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

std::string default_socket_path() {
    const char* user = std::getenv("USER");

    if (user != nullptr && user[0] != '\0') {
        return "/tmp/aiws-" +
               std::string(user) +
               ".sock";
    }

    return "/tmp/aiws-" +
           std::to_string(
               static_cast<unsigned long long>(
                   getuid())) +
           ".sock";
}

bool write_all(int fd,
               const void* buffer,
               std::size_t size) {
    const char* data = static_cast<const char*>(buffer);
    std::size_t total = 0;

    while (total < size) {
        const ssize_t count =
            write(fd, data + total, size - total);

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (count == 0) {
            return false;
        }

        total += static_cast<std::size_t>(count);
    }

    return true;
}

bool read_exact(int fd,
                void* buffer,
                std::size_t size) {
    char* data = static_cast<char*>(buffer);
    std::size_t total = 0;

    while (total < size) {
        const ssize_t count =
            read(fd, data + total, size - total);

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (count == 0) {
            return false;
        }

        total += static_cast<std::size_t>(count);
    }

    return true;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::string socket_path = default_socket_path();
    int argument_index = 1;

    if (argument_index < argc &&
        std::string(argv[argument_index]) == "--socket") {
        if (argument_index + 1 >= argc) {
            return 2;
        }

        socket_path = argv[argument_index + 1];
        argument_index += 2;
    }

    if (argument_index + 1 >= argc ||
        std::string(argv[argument_index]) != "add") {
        return 2;
    }

    const std::filesystem::path input_path =
        argv[argument_index + 1];

    const std::string file_path =
        std::filesystem::absolute(input_path).string();

    const int socket_fd =
        socket(AF_UNIX, SOCK_STREAM, 0);

    if (socket_fd < 0) {
        return 2;
    }

    timeval timeout{};
    timeout.tv_sec = 5;

    setsockopt(socket_fd,
               SOL_SOCKET,
               SO_RCVTIMEO,
               &timeout,
               sizeof(timeout));

    setsockopt(socket_fd,
               SOL_SOCKET,
               SO_SNDTIMEO,
               &timeout,
               sizeof(timeout));

    sockaddr_un address{};
    address.sun_family = AF_UNIX;

    if (socket_path.size() >= sizeof(address.sun_path)) {
        close(socket_fd);
        return 2;
    }

    std::memcpy(address.sun_path,
                socket_path.c_str(),
                socket_path.size() + 1);

    if (connect(socket_fd,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) < 0) {
        close(socket_fd);
        return 2;
    }

    const std::uint32_t path_length =
        static_cast<std::uint32_t>(
            file_path.size());

    const std::uint32_t network_length =
        htonl(path_length);

    if (!write_all(socket_fd,
                   &network_length,
                   sizeof(network_length)) ||
        !write_all(socket_fd,
                   file_path.data(),
                   file_path.size())) {
        close(socket_fd);
        return 2;
    }

    std::uint8_t status = 0;
    std::uint32_t network_message_length = 0;

    if (!read_exact(socket_fd,
                    &status,
                    sizeof(status)) ||
        !read_exact(socket_fd,
                    &network_message_length,
                    sizeof(network_message_length))) {
        close(socket_fd);
        return 2;
    }

    const std::uint32_t message_length =
        ntohl(network_message_length);

    std::string message(message_length, '\0');

    if (message_length > 0 &&
        !read_exact(socket_fd,
                    message.data(),
                    message.size())) {
        close(socket_fd);
        return 2;
    }

    close(socket_fd);

    if (status == 0) {
        return 0;
    }

    if (status == 1) {
        if (!message.empty()) {
            std::cerr << message << '\n';
        }

        return 1;
    }

    return 2;
}
