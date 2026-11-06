#include "aiws/gui/main_window.hpp"
#include "aiws/ipc_server.hpp"
#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QtTest>
#include <QCoreApplication>
#include <QProcess>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

QPushButton* find_button(QWidget& window, const QString& text) {
    const auto buttons = window.findChildren<QPushButton*>();

    for (QPushButton* button : buttons) {
        if (button->text() == text) {
            return button;
        }
    }

    return nullptr;
}

QLabel* find_corpus_label(QWidget& window) {
    const auto labels = window.findChildren<QLabel*>();

    for (QLabel* label : labels) {
        if (label->text().startsWith("Corpus:")) {
            return label;
        }
    }

    return nullptr;
}

QListWidget* find_document_list(QWidget& window) {
    return window.findChild<QListWidget*>();
}

QString create_file(QTemporaryDir& directory,
                    const QString& name) {
    const QString path = directory.filePath(name);

    QFile file(path);

    if (!file.open(QIODevice::WriteOnly)) {
        return {};
    }

    file.write("test document");
    file.close();

    return path;
}

bool write_all(int fd,
               const void* buffer,
               std::size_t size) {
    const char* data =
        static_cast<const char*>(buffer);

    std::size_t total = 0;

    while (total < size) {
        const ssize_t count =
            write(fd,
                  data + total,
                  size - total);

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (count == 0) {
            return false;
        }

        total +=
            static_cast<std::size_t>(count);
    }

    return true;
}

bool read_exact(int fd,
                void* buffer,
                std::size_t size) {
    char* data =
        static_cast<char*>(buffer);

    std::size_t total = 0;

    while (total < size) {
        const ssize_t count =
            read(fd,
                 data + total,
                 size - total);

        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }

        if (count == 0) {
            return false;
        }

        total +=
            static_cast<std::size_t>(count);
    }

    return true;
}

int send_request(const QString& socket_path,
                 const QString& file_path,
                 bool partial = false) {
    const int fd =
        socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }

    timeval timeout{};
    timeout.tv_sec = 2;

    setsockopt(fd,
               SOL_SOCKET,
               SO_RCVTIMEO,
               &timeout,
               sizeof(timeout));

    setsockopt(fd,
               SOL_SOCKET,
               SO_SNDTIMEO,
               &timeout,
               sizeof(timeout));

    sockaddr_un address{};
    address.sun_family = AF_UNIX;

    const QByteArray socket_bytes =
        socket_path.toUtf8();

    if (socket_bytes.size() >=
        static_cast<int>(
            sizeof(address.sun_path))) {
        close(fd);
        return -1;
    }

    std::memcpy(
        address.sun_path,
        socket_bytes.constData(),
        static_cast<std::size_t>(
            socket_bytes.size()) + 1);

    if (connect(
            fd,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)) < 0) {
        close(fd);
        return -1;
    }

    const QByteArray path_bytes =
        file_path.toUtf8();

    const std::uint32_t network_length =
        htonl(
            static_cast<std::uint32_t>(
                path_bytes.size()));

    bool sent = true;

    if (partial) {
        const char* length_bytes =
            reinterpret_cast<const char*>(
                &network_length);

        sent =
            write_all(fd,
                      length_bytes,
                      2);

        std::this_thread::sleep_for(
            std::chrono::milliseconds(20));

        if (sent) {
            sent =
                write_all(
                    fd,
                    length_bytes + 2,
                    sizeof(network_length) - 2);
        }

        const int middle =
            path_bytes.size() / 2;

        if (sent && middle > 0) {
            sent =
                write_all(
                    fd,
                    path_bytes.constData(),
                    static_cast<std::size_t>(
                        middle));

            std::this_thread::sleep_for(
                std::chrono::milliseconds(20));

            if (sent) {
                sent =
                    write_all(
                        fd,
                        path_bytes.constData()
                            + middle,
                        static_cast<std::size_t>(
                            path_bytes.size()
                            - middle));
            }
        }
    }
    else {
        sent =
            write_all(
                fd,
                &network_length,
                sizeof(network_length));

        if (sent) {
            sent =
                write_all(
                    fd,
                    path_bytes.constData(),
                    static_cast<std::size_t>(
                        path_bytes.size()));
        }
    }

    if (!sent) {
        close(fd);
        return -1;
    }

    std::uint8_t status = 0;
    std::uint32_t network_message_length = 0;

    if (!read_exact(fd,
                    &status,
                    sizeof(status)) ||
        !read_exact(
            fd,
            &network_message_length,
            sizeof(network_message_length))) {
        close(fd);
        return -1;
    }

    const std::uint32_t message_length =
        ntohl(network_message_length);

    std::string message(
        message_length,
        '\0');

    if (message_length > 0 &&
        !read_exact(
            fd,
            message.data(),
            message.size())) {
        close(fd);
        return -1;
    }

    close(fd);

    return static_cast<int>(status);
}

QString unique_socket_path(
    const QString& name) {
    static int counter = 0;

    return QString(
        "/tmp/aiws-test-%1-%2-%3.sock")
        .arg(
            static_cast<long long>(
                getpid()))
        .arg(name)
        .arg(++counter);
}

}  // namespace

class M4Tests : public QObject {
    Q_OBJECT

private slots:
    void responsive_build();
    void stale_build_after_add();
    void stale_build_after_reset();
    void build_failure();
    void lifecycle_during_build();
    void repeated_builds();

    void remote_add();
    void special_path();
    void partial_request();
    void server_shutdown();
};

void M4Tests::responsive_build() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString first =
        create_file(directory, "first.txt");

    const QString second =
        create_file(directory, "second.txt");

    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());

    aiws::gui::MainWindow window;
    window.set_build_delay_ms(600);

    QVERIFY(
        window.add_document_file(first));

    QPushButton* build =
        find_button(
            window,
            "Build Corpus");

    QLabel* corpus =
        find_corpus_label(window);

    QListWidget* list =
        find_document_list(window);

    QVERIFY(build != nullptr);
    QVERIFY(corpus != nullptr);
    QVERIFY(list != nullptr);

    build->click();

    QCOMPARE(
        corpus->text(),
        QString("Corpus: Building"));

    QVERIFY(
        window.add_document_file(second));

    QCOMPARE(list->count(), 2);
}

void M4Tests::stale_build_after_add() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString first =
        create_file(directory, "first.txt");

    const QString second =
        create_file(directory, "second.txt");

    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());

    aiws::gui::MainWindow window;
    window.set_build_delay_ms(300);

    QVERIFY(
        window.add_document_file(first));

    QPushButton* build =
        find_button(
            window,
            "Build Corpus");

    QLabel* corpus =
        find_corpus_label(window);

    QVERIFY(build != nullptr);
    QVERIFY(corpus != nullptr);

    build->click();

    QVERIFY(
        window.add_document_file(second));

    QTRY_VERIFY(build->isEnabled());

    QCOMPARE(
        corpus->text(),
        QString("Corpus: Not built"));
}

void M4Tests::stale_build_after_reset() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString first =
        create_file(directory, "first.txt");

    const QString second =
        create_file(directory, "second.txt");

    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());

    aiws::gui::MainWindow window;
    window.set_build_delay_ms(300);

    QVERIFY(
        window.add_document_file(first));

    QPushButton* build =
        find_button(
            window,
            "Build Corpus");

    QPushButton* reset =
        find_button(
            window,
            "Reset");

    QLabel* corpus =
        find_corpus_label(window);

    QListWidget* list =
        find_document_list(window);

    QVERIFY(build != nullptr);
    QVERIFY(reset != nullptr);
    QVERIFY(corpus != nullptr);
    QVERIFY(list != nullptr);

    build->click();
    reset->click();

    QCOMPARE(list->count(), 0);

    QCOMPARE(
        corpus->text(),
        QString("Corpus: Not built"));

    QVERIFY(
        window.add_document_file(second));

    QVERIFY(!build->isEnabled());

    QTRY_VERIFY(build->isEnabled());

    QCOMPARE(
        corpus->text(),
        QString("Corpus: Not built"));
}

void M4Tests::build_failure() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path =
        create_file(
            directory,
            "duplicate.txt");

    QVERIFY(!path.isEmpty());

    aiws::gui::MainWindow window;

    QVERIFY(
        window.add_document_file(path));

    QVERIFY(
        window.add_document_file(path));

    QPushButton* build =
        find_button(
            window,
            "Build Corpus");

    QLabel* corpus =
        find_corpus_label(window);

    QVERIFY(build != nullptr);
    QVERIFY(corpus != nullptr);

    build->click();

    QTRY_COMPARE(
        corpus->text(),
        QString("Corpus: Not built"));

    QTRY_VERIFY(
        build->isEnabled());
}

void M4Tests::lifecycle_during_build() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path =
        create_file(
            directory,
            "lifecycle.txt");

    QVERIFY(!path.isEmpty());

    QElapsedTimer timer;

    {
        aiws::gui::MainWindow window;

        window.set_build_delay_ms(200);

        QVERIFY(
            window.add_document_file(path));

        QPushButton* build =
            find_button(
                window,
                "Build Corpus");

        QVERIFY(build != nullptr);

        build->click();

        timer.start();
    }

    QVERIFY(timer.elapsed() < 1000);
}

void M4Tests::repeated_builds() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString first =
        create_file(directory, "one.txt");

    const QString second =
        create_file(directory, "two.txt");

    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());

    aiws::gui::MainWindow window;
    window.set_build_delay_ms(50);

    QVERIFY(
        window.add_document_file(first));

    QPushButton* build =
        find_button(
            window,
            "Build Corpus");

    QLabel* corpus =
        find_corpus_label(window);

    QVERIFY(build != nullptr);
    QVERIFY(corpus != nullptr);

    build->click();

    QTRY_VERIFY(
        corpus->text().contains("chunks"));

    QVERIFY(
        window.add_document_file(second));

    build->click();

    QTRY_VERIFY(
        corpus->text().contains("chunks"));
}

void M4Tests::remote_add() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString first =
        create_file(directory, "first.txt");

    const QString second =
        create_file(directory, "second.txt");

    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());

    aiws::gui::MainWindow window;
    window.set_build_delay_ms(1500);

    QVERIFY(
        window.add_document_file(first));

    QPushButton* build =
        find_button(
            window,
            "Build Corpus");

    QLabel* corpus =
        find_corpus_label(window);

    QListWidget* list =
        find_document_list(window);

    QVERIFY(build != nullptr);
    QVERIFY(corpus != nullptr);
    QVERIFY(list != nullptr);

    const QString socket_path =
        unique_socket_path("remote");

    aiws::IpcServer server(
        socket_path.toStdString(),
        &window);

    QVERIFY(server.start());

    build->click();

    QProcess sender;

    sender.setWorkingDirectory(
        directory.path());

    const QString sender_path =
        QCoreApplication::applicationDirPath()
        + "/aiws_send";

    sender.start(
        sender_path,
        QStringList{
            "--socket",
            socket_path,
            "add",
            "second.txt"
        });

    QVERIFY(sender.waitForStarted(1000));
    QVERIFY(sender.waitForFinished(5000));

    QCOMPARE(sender.exitCode(), 0);

    QTRY_COMPARE(
        list->count(),
        2);

    QTRY_VERIFY(
        build->isEnabled());

    QCOMPARE(
        corpus->text(),
        QString("Corpus: Not built"));

    server.stop();
}

void M4Tests::special_path() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString name =
        QString::fromUtf8(
            "caf\xC3\xA9 file\nname.txt");

    const QString path =
        create_file(
            directory,
            name);

    QVERIFY(!path.isEmpty());

    aiws::gui::MainWindow window;

    const QString socket_path =
        unique_socket_path("special");

    aiws::IpcServer server(
        socket_path.toStdString(),
        &window);

    QVERIFY(server.start());

    QCOMPARE(
        send_request(
            socket_path,
            path),
        0);

    QListWidget* list =
        find_document_list(window);

    QVERIFY(list != nullptr);

    QTRY_COMPARE(
        list->count(),
        1);

    server.stop();
}

void M4Tests::partial_request() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    const QString path =
        create_file(
            directory,
            "partial.txt");

    QVERIFY(!path.isEmpty());

    aiws::gui::MainWindow window;

    const QString socket_path =
        unique_socket_path("partial");

    aiws::IpcServer server(
        socket_path.toStdString(),
        &window);

    QVERIFY(server.start());

    QCOMPARE(
        send_request(
            socket_path,
            path,
            true),
        0);

    QListWidget* list =
        find_document_list(window);

    QVERIFY(list != nullptr);

    QTRY_COMPARE(
        list->count(),
        1);

    server.stop();
}

void M4Tests::server_shutdown() {
    aiws::gui::MainWindow window;

    const QString socket_path =
        unique_socket_path("shutdown");

    aiws::IpcServer server(
        socket_path.toStdString(),
        &window);

    QVERIFY(server.start());

    QVERIFY(
        QFileInfo::exists(
            socket_path));

    QElapsedTimer timer;
    timer.start();

    server.stop();

    QVERIFY(timer.elapsed() < 500);

    QVERIFY(
        !QFileInfo::exists(
            socket_path));
}

QTEST_MAIN(M4Tests)

#include "student_tests.moc"
