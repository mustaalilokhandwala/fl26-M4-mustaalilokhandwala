#include "aiws/ipc_server.hpp"
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>

namespace {

bool read_exact(int fd,
                void* buffer,
                std::size_t size,
                const std::atomic<bool>& stop_requested) {
    char* data = static_cast<char*>(buffer);
    std::size_t total = 0;

    while (total < size && !stop_requested.load()) {
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLIN;

        const int ready = poll(&descriptor, 1, 100);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (ready == 0) {
            continue;
        }

        const ssize_t count =
            read(fd, data + total, size - total);

        if (count <= 0) {
            if (count < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }

        total += static_cast<std::size_t>(count);
    }

    return total == size;
}

bool write_all(int fd,
               const void* buffer,
               std::size_t size,
               const std::atomic<bool>& stop_requested) {
    const char* data = static_cast<const char*>(buffer);
    std::size_t total = 0;

    while (total < size && !stop_requested.load()) {
        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = POLLOUT;

        const int ready = poll(&descriptor, 1, 100);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (ready == 0) {
            continue;
        }

        const ssize_t count =
            write(fd, data + total, size - total);

        if (count <= 0) {
            if (count < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }

        total += static_cast<std::size_t>(count);
    }

    return total == size;
}

}  // namespace

namespace aiws {

IpcServer::IpcServer(std::string socket_path, QObject* receiver)
    : socket_path_(std::move(socket_path)),
      receiver_(receiver) {
}

IpcServer::~IpcServer() {
    stop();
}

bool IpcServer::start() {
    if (thread_.joinable()) {
        return false;
    }

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);

    if (listen_fd_ < 0) {
        return false;
    }

    sockaddr_un address{};
    address.sun_family = AF_UNIX;

    if (socket_path_.size() >= sizeof(address.sun_path)) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    std::memcpy(address.sun_path,
                socket_path_.c_str(),
                socket_path_.size() + 1);

    if (bind(listen_fd_,
             reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, 5) < 0) {
        close(listen_fd_);
        listen_fd_ = -1;
        unlink(socket_path_.c_str());
        return false;
    }

    stop_requested_ = false;
    thread_ = std::thread(&IpcServer::run, this);

    return true;
}

void IpcServer::stop() {
    stop_requested_ = true;

    if (thread_.joinable()) {
        thread_.join();
    }
}

void IpcServer::run() {
    while (!stop_requested_.load()) {
        pollfd descriptor{};
        descriptor.fd = listen_fd_;
        descriptor.events = POLLIN;

        const int ready = poll(&descriptor, 1, 100);

        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (ready == 0) {
            continue;
        }

        const int client_fd =
            accept(listen_fd_, nullptr, nullptr);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            continue;
        }

        std::uint32_t network_length = 0;

        if (!read_exact(client_fd,
                        &network_length,
                        sizeof(network_length),
                        stop_requested_)) {
            close(client_fd);
            continue;
        }

        const std::uint32_t path_length =
            ntohl(network_length);

        std::string path(path_length, '\0');

        if (path_length > 0 &&
            !read_exact(client_fd,
                        path.data(),
                        path.size(),
                        stop_requested_)) {
            close(client_fd);
            continue;
        }

        std::ifstream file(path);
        const bool readable = file.is_open();

        const std::uint8_t status =
            readable ? 0 : 1;

        const std::string message =
            readable ? "" : "Unable to read file.";

        const std::uint32_t message_length =
            htonl(static_cast<std::uint32_t>(
                message.size()));

        bool reply_sent =
            write_all(client_fd,
                      &status,
                      sizeof(status),
                      stop_requested_);

        if (reply_sent) {
            reply_sent =
                write_all(client_fd,
                          &message_length,
                          sizeof(message_length),
                          stop_requested_);
        }

        if (reply_sent && !message.empty()) {
            reply_sent =
                write_all(client_fd,
                          message.data(),
                          message.size(),
                          stop_requested_);
        }

        close(client_fd);

        if (readable &&
            reply_sent &&
            !stop_requested_.load()) {
            const QString qpath =
                QString::fromUtf8(
                    path.data(),
                    static_cast<int>(path.size()));

            QMetaObject::invokeMethod(
                receiver_,
                "remote_add_document",
                Qt::QueuedConnection,
                Q_ARG(QString, qpath));
        }
    }

    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }

    unlink(socket_path_.c_str());
}

}  // namespace aiws
