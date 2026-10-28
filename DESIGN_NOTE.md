# M4 Design Note

# 1. Components and Threads
MainWindow remains the main application component and owns the Workspace, the current ProcessingCore, and the GUI widgets. All MainWindow slots and all widget operations run on the GUI thread. A background std::thread performs each corpus build. At the start of a build, it receives a copy of the current Workspace, the current workspace generation number, and the build delay value. The worker builds into its own temporary ProcessingCore and never reads or changes GUI widgets. When the build finishes, the result is sent back to the GUI thread using a queued Qt call. The GUI thread decides whether the completed build is still valid before using it.

# 2. Corpus States
The corpus states are:
Empty: there are no documents and no usable corpus.
Stale: the Workspace has changed and the existing corpus is no longer current.
Building: a corpus build is running in the background.
Current: the corpus matches the present Workspace.
Adding a document changes the workspace generation and makes the corpus stale. Build Corpus changes the state to Building. A successful valid build changes it to Current. Reset changes the state to Empty. Build Corpus is disabled while a build is already running.

# 3. Event Sequences and Risks
R1 is threatened if the corpus build or build delay runs on the GUI thread. Both run only on the background thread. R2 is threatened if the Workspace changes while a build is running. Every Workspace change increments a generation number. A build remembers the generation it started with. Its result is accepted only if that generation still matches the current generation.
Example 1: generation 4 starts building. The user adds a document, changing the generation to 5. Generation 4 finishes. Since the generations do not match, its result is discarded and the corpus remains stale.
Example 2: a build is running and the user clicks Reset. Reset increments the generation, clears the Workspace and output, and changes the state to Empty. When the old build finishes, its generation does not match, so its result is discarded.
R3 is handled by reporting a build failure back to the GUI thread. The application leaves the building state and remains usable. R4 is handled by never detaching the worker thread and joining it before its owner is destroyed.

# 4. Corpus Ownership
The worker creates a temporary ProcessingCore for the new corpus. When the build finishes, the result is returned to the GUI thread. If its generation is still current, it replaces the previous corpus. If it is stale, it is discarded. The accepted corpus remains owned by MainWindow until it is replaced, Reset is clicked, or the application exits.

# 5. Rejected Alternative
I rejected rebuilding the existing MainWindow ProcessingCore directly on the background thread. The GUI could try to use the same object while the worker is changing it. Using a separate temporary ProcessingCore keeps the build separate from the current corpus and allows stale results to be discarded safely.
