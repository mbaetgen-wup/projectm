/**
 * @file PresetCpuWorker.hpp
 * @brief Single background thread that performs CPU-bound preset file reading.
 */
#pragma once

#include "PresetSwitchContext.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace libprojectM {

/**
 * @brief Runs a single dedicated thread for CPU-bound preset file I/O.
 *
 * Responsibilities (CPU only – never touches GL or preset construction):
 *  - Reading the preset file from disk into memory.
 *
 * All preset construction, GL resource creation, shader compilation and
 * activation remain on the render thread.
 *
 * At most one pending load exists at a time.  Submitting a new context
 * implicitly cancels any prior pending context.
 */
class PresetCpuWorker
{
public:
    /**
     * @brief Constructs the worker and starts the background thread.
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
     * @param ctx Shared ownership of the switch context.
     */
    void StartLoad(std::shared_ptr<PresetSwitchContext> ctx);

private:
    void ThreadLoop();

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;

    /// The next context to process.  Protected by m_mutex.
    std::shared_ptr<PresetSwitchContext> m_pending;

    /// Set to false to shut down the thread.
    bool m_running{true};
};

} // namespace libprojectM
