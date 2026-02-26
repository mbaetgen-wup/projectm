/**
 * @file PresetCpuWorker.hpp
 * @brief Single background thread that performs CPU-bound preset file reading
 *        and expression bytecode compilation.
 */
#pragma once

#include "PresetSwitchContext.hpp"

#include <memory>

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
#define PROJECTM_USE_WORKER_THREAD 1
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace libprojectM {

/**
 * @brief Runs a single dedicated thread for CPU-bound preset work.
 *
 * Responsibilities (CPU only – never touches GL or GL resource creation):
 *  - Reading the preset file from disk into memory.
 *  - Compiling projectm-eval bytecode expressions after the render thread
 *    has constructed the preset (overlaps with current-preset rendering).
 *
 * All preset construction, GL resource creation, shader compilation and
 * activation remain on the render thread.
 *
 * On platforms without threading support (e.g. Emscripten without pthreads),
 * all work executes synchronously in the calling thread.
 *
 * At most one pending load exists at a time.  Submitting a new context
 * implicitly cancels any prior pending context.
 */
class PresetCpuWorker
{
public:
    /**
     * @brief Constructs the worker and starts the background thread (if available).
     */
    PresetCpuWorker();

    /**
     * @brief Signals the thread to stop and joins it.
     */
    ~PresetCpuWorker();

    PresetCpuWorker(const PresetCpuWorker&) = delete;
    PresetCpuWorker& operator=(const PresetCpuWorker&) = delete;

    /**
     * @brief Submits a new preset file-read request.
     *
     * Any previously pending (but not yet started) load is cancelled.
     * If a load is currently in progress, the old context's cancelled flag
     * is set so the worker will stop early.
     *
     * Without threading support, the file is read synchronously before returning.
     *
     * @param ctx Shared ownership of the switch context.
     */
    void StartLoad(std::shared_ptr<PresetSwitchContext> ctx);

    /**
     * @brief Submits an expression compilation request.
     *
     * Called by the render thread after constructing the preset (in GlStaging).
     * The worker will call preset->CompileExpressions() and advance the
     * context state to GlPhases when complete.
     *
     * Without threading support, expressions are compiled synchronously
     * before returning.
     *
     * @param ctx Shared ownership of the switch context (preset must be set).
     */
    void SubmitExpressionCompile(std::shared_ptr<PresetSwitchContext> ctx);

private:
#if PROJECTM_USE_WORKER_THREAD
    void ThreadLoop();

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;

    /// The next file-read context to process.  Protected by m_mutex.
    std::shared_ptr<PresetSwitchContext> m_pending;

    /// The next expression-compile context to process.  Protected by m_mutex.
    std::shared_ptr<PresetSwitchContext> m_expressionCtx;

    /// Currently-processing contexts (copies held by ThreadLoop so
    /// the destructor can cancel in-flight work).  Protected by m_mutex.
    std::shared_ptr<PresetSwitchContext> m_activeFileCtx;
    std::shared_ptr<PresetSwitchContext> m_activeExprCtx;

    /// Set to false to shut down the thread.
    bool m_running{true};
#endif

    /// File I/O logic, shared by threaded and synchronous paths.
    static void DoFileRead(std::shared_ptr<PresetSwitchContext>& ctx);

    /// Expression compilation logic, shared by threaded and synchronous paths.
    static void DoExpressionCompile(std::shared_ptr<PresetSwitchContext>& ctx);
};

} // namespace libprojectM
