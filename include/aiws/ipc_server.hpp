#pragma once

#include <atomic>
#include <string>
#include <thread>

class QObject;

namespace aiws {

class IpcServer {
public:
    IpcServer(std::string socket_path, QObject* receiver);
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    bool start();
    void stop();

private:
    void run();

    std::string socket_path_;
    QObject* receiver_{nullptr};

    std::atomic<bool> stop_requested_{false};
    std::thread thread_;

    int listen_fd_{-1};
};

}  // namespace aiws
