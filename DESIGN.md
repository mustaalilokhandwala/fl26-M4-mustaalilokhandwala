# M4 Design

## Threads and Ownership
MainWindow and all widgets run on the GUI thread. MainWindow owns the Workspace, the current ProcessingCore, and the background build thread. MainWindow::build_corpus() copies the current Workspace, generation number, and build delay and starts MainWindow::run_build() on a std::thread. The worker uses only its copied Workspace and creates a temporary ProcessingCore. It does not read or modify GUI widgets. The result is transferred through std::promise/std::future, and QMetaObject::invokeMethod() queues build_finished() on the GUI thread. For constraint 5, the background build does not share mutable Workspace or corpus data with the GUI thread because it receives copies. In IpcServer, stop_requested_ is the only mutable value used by both the owner thread and IPC thread, and it is a std::atomic<bool>. The IPC thread owns its socket work while running. A successfully accepted build is moved into MainWindow::processing_core_. A stale or failed temporary build is destroyed without replacing the current core. On Reset, the existing core is marked unusable by corpus_current_ = false; processing actions cannot use it. It is later replaced by a valid build or destroyed with MainWindow.

## Build Validity
Each Workspace change increments workspace_generation_. A build saves the generation that existed when it started. When build_finished() runs, the result is accepted only when its saved generation equals the current generation. If a document was added or Reset occurred during the build, the generations differ and the result is discarded. A build failure is returned in BuildResult::error. The GUI leaves the building state, reports the failure, and remains usable.

## Changes Since the Checkpoint
The background-build design stayed the same as DESIGN_NOTE.md: a copied Workspace is built on a worker thread and generation numbers reject stale results. One implementation detail differs from the checkpoint wording. Reset invalidates the existing corpus instead of immediately destroying the ProcessingCore. This is safe because corpus_current_ becomes false, so Search, Term Inspector, and Context cannot use that core. CR-1 also added the IPC components described below.

## CR-1 Remote Add
CR-1 added IpcServer, MainWindow::remote_add_document(), the --ipc-socket option in main(), and the aiws_send program under tools/. remote_add_document() calls the existing add_document_file(), so remote adds use the same Workspace update, generation increment, and corpus invalidation path as GUI adds. The existing search, context, indexing, and widget code did not need to change. IpcServer receives requests on its own std::thread. It checks whether the requested file is readable and sends its reply before queuing remote_add_document() to the GUI thread. The request format is a 4-byte unsigned path length in network byte order followed by exactly that many UTF-8 path bytes. The response is a 1-byte status, followed by a 4-byte unsigned message length in network byte order, followed by that many message bytes. Status 0 means accepted and status 1 means rejected. The length prefix allows paths containing spaces, UTF-8 characters, and newlines.

## Fault Switches and Tests
AIWS_FAULT_ACCEPT_STALE is located in MainWindow::build_finished() where build validity is decided. stale_build_after_add() and stale_build_after_reset() detect it; remote_add() also detects the same stale-result behavior for CR-1. AIWS_FAULT_LOSE_FAILURE is located in MainWindow::run_build() where completion is delivered to the GUI. build_failure() detects it because the application remains in the Building state instead of reporting the failure.

## AI Use
I used AI as an assistant to review the assignment requirements, discuss design choices, and help debug parts of my M4 implementation and tests. I reviewed, edited, built, and tested the submitted code myself.

## Pair programming log
Partner: Anish Kankanala

Session 1: November 6, 2026, 9:15 AM–10:00 AM. We worked on my M4 code, mainly the background build, stale-generation handling, and CR-1 remote add behavior. I drove from 9:15 AM to 9:40 AM while walking through build_corpus(), run_build(), build_finished(), and how Workspace changes invalidate a running build. We swapped roles at 9:40 AM, and Anish drove from 9:40 AM to 10:00 AM while I navigated through the related tests and fault behavior. Anish’s navigator contribution was reviewing the stale-build path and reminding me that a remote add during an active build needed to invalidate the old build the same way as a GUI add.

Session 2: November 6, 2026, 9:40 PM–10:20 PM. We worked on Anish’s M4 code, mainly his background-build and CR-1 integration. Anish drove from 9:40 PM to 10:00 PM while I navigated and reviewed the thread lifecycle, generation checking, and stale-result handling. We swapped roles at 10:00 PM, and I drove from 10:00 PM to 10:20 PM while we reviewed his tests and integration behavior. My navigator contribution was pointing out the need to handle and test the case where the Workspace changes while a corpus build is still running.

The interaction that worked best for us was having the driver explain each change while the navigator followed the control flow and called out edge cases before moving on.
