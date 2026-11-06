#include "aiws/gui/main_window.hpp"
#include "aiws/ipc_server.hpp"
#include <QApplication>
#include <QMessageBox>
#include <cstdlib>
#include <string>
#include <unistd.h>

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

void confirm_exit(QWidget* parent) {
    const auto choice =
        QMessageBox::question(parent,
                              "Confirm Exit",
                              "Are you sure you want to exit?",
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No);

    if (choice == QMessageBox::Yes) {
        QApplication::quit();
    }
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    int build_delay_ms = 0;
    std::string socket_path =
        default_socket_path();

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (argument == "--build-delay-ms" &&
            i + 1 < argc) {
            build_delay_ms =
                std::stoi(argv[++i]);
        }
        else if (argument == "--ipc-socket" &&
                 i + 1 < argc) {
            socket_path = argv[++i];
        }
    }

    aiws::gui::MainWindow window;
    window.set_build_delay_ms(build_delay_ms);
    window.set_exit_callback(&confirm_exit);

    aiws::IpcServer ipc_server(
        socket_path,
        &window);

    if (!ipc_server.start()) {
        return 1;
    }

    window.show();

    return app.exec();
}
