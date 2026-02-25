/**
 * @file PresetCpuWorker.cpp
 * @brief Implementation of the background CPU worker for preset file reading.
 */
#include "PresetCpuWorker.hpp"

#include "Logging.hpp"
#include "PresetFactory.hpp"

#include <fstream>
#include <sstream>

namespace libprojectM {

PresetCpuWorker::PresetCpuWorker()
    : m_thread(&PresetCpuWorker::ThreadLoop, this)
{
}

PresetCpuWorker::~PresetCpuWorker()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_running = false;
        if (m_pending)
        {
            m_pending->cancelled.store(true, std::memory_order_release);
        }
    }
    m_cv.notify_one();
    if (m_thread.joinable())
    {
        m_thread.join();
    }
}

void PresetCpuWorker::StartLoad(std::shared_ptr<PresetSwitchContext> ctx)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_pending)
        {
            m_pending->cancelled.store(true, std::memory_order_release);
        }
        m_pending = std::move(ctx);
    }
    m_cv.notify_one();
}

void PresetCpuWorker::ThreadLoop()
{
    while (true)
    {
        std::shared_ptr<PresetSwitchContext> ctx;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return !m_running || m_pending; });

            if (!m_running)
            {
                return;
            }

            ctx = std::move(m_pending);
        }

        if (!ctx || ctx->cancelled.load(std::memory_order_acquire))
        {
            continue;
        }

        // ---- CPU-bound work: read file contents into memory ----
        //
        // We only perform file I/O here.  Preset construction (which
        // involves GL resource creation) MUST happen on the render thread.
        try
        {
            // Resolve the protocol / path.  The idle preset and non-file
            // protocols are not handled here — they are fast enough to
            // load synchronously on the render thread.
            std::string resolvedPath;
            std::string protocol = PresetFactory::Protocol(ctx->path, resolvedPath);

            if (!protocol.empty() && protocol != "file")
            {
                // Non-file protocol (e.g. "idle://") — leave fileData empty
                // and mark as ready for GL staging.  The render thread will
                // use the standard synchronous factory path.
                ctx->state.store(PresetSwitchState::GlStaging, std::memory_order_release);
                continue;
            }

            if (ctx->cancelled.load(std::memory_order_acquire))
            {
                continue;
            }

            // Read the file into a string.
            std::ifstream file(resolvedPath.c_str(),
                               std::ios_base::in | std::ios_base::binary);
            if (!file.good())
            {
                ctx->errorMessage = "Could not open preset file: \"" + resolvedPath + "\".";
                ctx->state.store(PresetSwitchState::Failed, std::memory_order_release);
                continue;
            }

            file.seekg(0, std::ios::end);
            auto size = file.tellg();
            file.seekg(0, std::ios::beg);

            if (size <= 0 || static_cast<size_t>(size) > 0x100000)
            {
                ctx->errorMessage = "Preset file has invalid size: \"" + resolvedPath + "\".";
                ctx->state.store(PresetSwitchState::Failed, std::memory_order_release);
                continue;
            }

            std::string data(static_cast<size_t>(size), '\0');
            file.read(&data[0], size);

            if (file.fail() || file.bad())
            {
                ctx->errorMessage = "Failed to read preset file: \"" + resolvedPath + "\".";
                ctx->state.store(PresetSwitchState::Failed, std::memory_order_release);
                continue;
            }

            if (ctx->cancelled.load(std::memory_order_acquire))
            {
                continue;
            }

            // Store the raw file data and advance to GlStaging.
            ctx->fileData = std::move(data);
            ctx->state.store(PresetSwitchState::GlStaging, std::memory_order_release);
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR("[PresetCpuWorker] " + std::string(ex.what()));
            ctx->errorMessage = ex.what();
            ctx->state.store(PresetSwitchState::Failed, std::memory_order_release);
        }
        catch (...)
        {
            LOG_ERROR("[PresetCpuWorker] Unknown exception during preset file reading.");
            ctx->errorMessage = "Unknown exception during preset file reading.";
            ctx->state.store(PresetSwitchState::Failed, std::memory_order_release);
        }
    }
}

} // namespace libprojectM
