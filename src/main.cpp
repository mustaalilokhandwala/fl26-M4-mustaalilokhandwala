#include "aiws/gui/main_window.hpp"

#include <QApplication>
#include <QMessageBox>
#include <string>

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

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];

        if (argument == "--build-delay-ms" &&
            i + 1 < argc) {
            build_delay_ms = std::stoi(argv[++i]);
        }
    }

    aiws::gui::MainWindow window;
    window.set_build_delay_ms(build_delay_ms);
    window.set_exit_callback(&confirm_exit);
    window.show();

    return app.exec();
}
