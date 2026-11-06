#include "aiws/gui/main_window.hpp"
#include "aiws/document.hpp"
#include "aiws/gui/context_widget.hpp"
#include "aiws/gui/resettable.hpp"
#include "aiws/gui/search_widget.hpp"
#include "aiws/gui/term_inspector_widget.hpp"
#include "aiws/gui/workspace_widget.hpp"
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QString>
#include <QTextStream>
#include <QVBoxLayout>
#include <QWidget>
#include <cstddef>
#include <utility>
#include <QMetaObject>
#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <thread>

namespace aiws::gui {

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("AI Workspace");
    resize(900, 900);

    auto* central_widget = new QWidget(this);
    setCentralWidget(central_widget);

    auto* header_label =
        new QLabel("AI Workspace\nDocument Search and Context Explorer",
                   central_widget);
    header_label->setAlignment(Qt::AlignCenter);
    header_label->setStyleSheet(
        "background-color: #7a1f2b;"
        "color: white;"
        "padding: 10px;"
        "font-weight: bold;");

    workspace_widget_ = new WorkspaceWidget(central_widget);
    search_widget_ = new SearchWidget(central_widget);
    term_inspector_widget_ = new TermInspectorWidget(central_widget);
    context_widget_ = new ContextWidget(central_widget);

    status_label_ = new QLabel("Ready", central_widget);

    reset_button_ = new QPushButton("Reset", central_widget);
    exit_button_ = new QPushButton("Exit", central_widget);

    reset_button_->setStyleSheet(
        "background-color: #d6a53a;");
    exit_button_->setStyleSheet(
        "background-color: #3b6ca8;"
        "color: white;");

    auto* bottom_layout = new QHBoxLayout;
    bottom_layout->addWidget(term_inspector_widget_);
    bottom_layout->addWidget(context_widget_);

    auto* status_title = new QLabel("Current status:", central_widget);

    auto* status_layout = new QHBoxLayout;
    status_layout->addWidget(status_title);
    status_layout->addWidget(status_label_);
    status_layout->addStretch();
    status_layout->addWidget(reset_button_);
    status_layout->addWidget(exit_button_);

    auto* main_layout = new QVBoxLayout(central_widget);
    main_layout->addWidget(header_label);
    main_layout->addWidget(workspace_widget_);
    main_layout->addWidget(search_widget_);
    main_layout->addLayout(bottom_layout);
    main_layout->addLayout(status_layout);

    connect(workspace_widget_,
            &WorkspaceWidget::add_document_requested,
            this,
            &MainWindow::add_document);

    connect(workspace_widget_,
            &WorkspaceWidget::build_corpus_requested,
            this,
            &MainWindow::build_corpus);

    connect(workspace_widget_,
            &WorkspaceWidget::document_selected,
            this,
            &MainWindow::document_selected);

    connect(search_widget_,
            &SearchWidget::search_requested,
            this,
            &MainWindow::search_requested);

    connect(search_widget_,
            &SearchWidget::query_changed,
            this,
            &MainWindow::query_changed);

    connect(search_widget_,
            &SearchWidget::source_document_requested,
            this,
            &MainWindow::source_document_requested);

    connect(search_widget_,
            &SearchWidget::selected_chunk_changed,
            term_inspector_widget_,
            &TermInspectorWidget::set_selected_chunk);

    connect(term_inspector_widget_,
            &TermInspectorWidget::term_inspection_requested,
            this,
            &MainWindow::term_inspection_requested);

    connect(context_widget_,
            &ContextWidget::build_context_requested,
            this,
            &MainWindow::build_context_requested);

    connect(reset_button_,
            &QPushButton::clicked,
            this,
            &MainWindow::reset_application);

    connect(exit_button_,
            &QPushButton::clicked,
            this,
            &MainWindow::exit_application);

    update_action_states();
}

MainWindow::~MainWindow() {
    if (build_thread_.joinable()) {
        build_thread_.join();
    }
}

void MainWindow::set_exit_callback(ExitCallback callback) noexcept {
    exit_callback_ = callback;
}

void MainWindow::set_build_delay_ms(int ms_per_document) {
    if (ms_per_document < 0) {
        ms_per_document = 0;
    }

    build_delay_ms_ = ms_per_document;
}

bool MainWindow::add_document_file(const QString& path) {
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream stream(&file);
    const QString text = stream.readAll();

    QFileInfo file_info(path);
    const QString document_id = file_info.absoluteFilePath();
    const QString title = file_info.fileName();

    workspace_.add_document(
        Document(document_id.toStdString(),
                 title.toStdString(),
                 text.toStdString()));

    workspace_widget_->add_document(title);

    ++workspace_generation_;
    invalidate_corpus();

    status_label_->setText("Document added.");

    return true;
}

void MainWindow::add_document() {
    const QString file_path =
        QFileDialog::getOpenFileName(this,
                                     "Add Document",
                                     QString(),
                                     "Text Files (*.txt)");

    if (file_path.isEmpty()) {
        return;
    }

    if (!add_document_file(file_path)) {
        status_label_->setText("Unable to open document.");
    }
}

void MainWindow::build_corpus() {
    if (workspace_.documents().empty() || build_running_) {
        return;
    }

    Workspace workspace_copy = workspace_;
    const std::size_t generation = workspace_generation_;
    const int delay_ms = build_delay_ms_;

    std::promise<BuildResult> promise;
    build_future_ = promise.get_future();

    build_running_ = true;
    corpus_current_ = false;

    clear_processing_outputs();

    workspace_widget_->set_corpus_status("Corpus: Building");
    status_label_->setText("Building corpus.");

    update_action_states();

    build_thread_ =
        std::thread(&MainWindow::run_build,
                    this,
                    std::move(workspace_copy),
                    generation,
                    delay_ms,
                    std::move(promise));
}

void MainWindow::document_selected(int row) {
    if (row < 0 ||
        static_cast<std::size_t>(row) >= workspace_.documents().size()) {
        workspace_widget_->clear_selected_document();
        return;
    }

    const Document& document =
        workspace_.documents()[static_cast<std::size_t>(row)];

    workspace_widget_->show_selected_document(
        QString::fromStdString(document.title()),
        QString::fromStdString(document.text()));
}

void MainWindow::search_requested(const QString& query, int k) {
    if (!corpus_current_ || query.trimmed().isEmpty()) {
        return;
    }

    auto results =
        processing_core_.search(query.toStdString(), k);

    search_widget_->set_results(std::move(results));

    term_inspector_widget_->clear_statistics();
    context_widget_->clear_context();

    status_label_->setText("Search complete.");

    update_action_states();
}

void MainWindow::query_changed(const QString& query) {
    (void)query;
    update_action_states();
}

void MainWindow::build_context_requested(int token_budget) {
    if (!corpus_current_ || !search_widget_->has_query()) {
        return;
    }

    const auto items =
        processing_core_.build_context(
            search_widget_->query().toStdString(),
            search_widget_->result_count(),
            static_cast<std::size_t>(token_budget));

    context_widget_->set_context(items);

    status_label_->setText("Context built.");
}

void MainWindow::source_document_requested(const QString& document_id) {
    const std::string id = document_id.toStdString();
    const auto& documents = workspace_.documents();

    for (std::size_t i = 0; i < documents.size(); ++i) {
        if (documents[i].id() == id) {
            workspace_widget_->select_document(
                static_cast<int>(i));
            return;
        }
    }
}

void MainWindow::term_inspection_requested(const QString& term,
                                           const QString& chunk_id) {
    if (!corpus_current_ || term.trimmed().isEmpty()) {
        return;
    }

    const std::size_t document_frequency =
        processing_core_.document_frequency(term.toStdString());

    const bool has_chunk_frequency = !chunk_id.isEmpty();

    std::size_t chunk_frequency = 0;

    if (has_chunk_frequency) {
        chunk_frequency =
            processing_core_.term_frequency(
                term.toStdString(),
                chunk_id.toStdString());
    }

    term_inspector_widget_->set_statistics(
        document_frequency,
        has_chunk_frequency,
        chunk_frequency);

    status_label_->setText("Term inspected.");
}

void MainWindow::reset_application() {
    ++workspace_generation_;
    workspace_.clear();
    corpus_current_ = false;

    Resettable* components[] = {
        workspace_widget_,
        search_widget_,
        term_inspector_widget_,
        context_widget_
    };

    for (Resettable* component : components) {
        component->reset();
    }

    status_label_->setText("Application reset.");

    update_action_states();
}

void MainWindow::exit_application() {
    if (exit_callback_ != nullptr) {
        exit_callback_(this);
    }
}

void MainWindow::update_action_states() {
    const bool has_documents =
        !workspace_.documents().empty();

    const bool processing_available =
        corpus_current_ && search_widget_->has_query();

    workspace_widget_->set_build_corpus_enabled(
        has_documents && !build_running_);

    search_widget_->set_search_enabled(processing_available);

    term_inspector_widget_->set_inspection_enabled(
        corpus_current_);

    context_widget_->set_build_context_enabled(
        processing_available);
}

void MainWindow::invalidate_corpus() {
    corpus_current_ = false;

    workspace_widget_->set_corpus_status(
        "Corpus: Not built");

    clear_processing_outputs();

    update_action_states();
}

void MainWindow::clear_processing_outputs() {
    search_widget_->clear_results();
    term_inspector_widget_->clear_statistics();
    context_widget_->clear_context();
}

void MainWindow::run_build(
    Workspace workspace,
    std::size_t generation,
    int delay_ms,
    std::promise<BuildResult> promise) {

    BuildResult result;
    result.generation = generation;

    try {
        if (delay_ms > 0) {
            for (std::size_t i = 0;
                 i < workspace.documents().size();
                 ++i) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(delay_ms));
            }
        }

        result.core = std::make_unique<ProcessingCore>();
        result.core->rebuild(workspace);
    }
    catch (const std::exception& error) {
        result.error = error.what();
    }

    const bool failed = !result.error.empty();

    promise.set_value(std::move(result));

#ifdef AIWS_FAULT_LOSE_FAILURE
    if (failed) {
        return;
    }
#else
    (void)failed;
#endif

    QMetaObject::invokeMethod(
        this,
        "build_finished",
        Qt::QueuedConnection);
}

void MainWindow::remote_add_document(const QString& path) {
    add_document_file(path);
}

void MainWindow::build_finished() {
    if (build_thread_.joinable()) {
        build_thread_.join();
    }

    BuildResult result = build_future_.get();

    build_running_ = false;

    if (!result.error.empty()) {
        corpus_current_ = false;

        workspace_widget_->set_corpus_status(
            "Corpus: Not built");

        status_label_->setText(
            QString("Build failed: %1")
                .arg(QString::fromStdString(result.error)));

        update_action_states();
        return;
    }

    bool generation_matches =
        result.generation == workspace_generation_;

#ifdef AIWS_FAULT_ACCEPT_STALE
    generation_matches = true;
#endif

    if (!generation_matches) {
        corpus_current_ = false;
        update_action_states();
        return;
    }

    processing_core_ = std::move(*result.core);
    corpus_current_ = true;

    clear_processing_outputs();

    workspace_widget_->set_corpus_status(
        QString("Corpus: %1 chunks")
            .arg(static_cast<unsigned long long>(
                processing_core_.chunk_count())));

    status_label_->setText("Corpus built.");

    update_action_states();
}

}  // namespace aiws::gui
