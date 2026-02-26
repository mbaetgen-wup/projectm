/**
 * @file PresetCpuWorker.cpp
 * @brief Implementation of the background CPU worker for preset file reading
 *        and expression bytecode compilation.
 */
#include "PresetCpuWorker.hpp"

#include "Logging.hpp"
#include "Preset.hpp"
#include "PresetFactory.hpp"

#include <fstream>
#include <sstream>

namespace libprojectM {

// ---- Shared work routines (used by both threaded and synchronous paths) ----

void PresetCpuWorker::DoExpressionCompile(std::shared_ptr<PresetSwitchContext>& ctx)
{
    if (!ctx || ctx->cancelled.load(std::memory_order_acquire))
    {
        return;
    }

    try
    {
        if (ctx->preset)
        {
            ctx->preset->CompileExpressions();

            // Check cancellation after the (potentially long) compile.
            if (ctx->cancelled.load(std::memory_order_acquire))
            {
                return;
            }

            // Pre-decode texture image files referenced by the shaders.
            // This is CPU-only work (stbi_load) that avoids synchronous
            // disk I/O on the render thread during Phase 1.
            if (ctx->textureManager)
            {
                ctx->preset->PreloadTextures(ctx->textureManager);
            }

            if (ctx->cancelled.load(std::memory_order_acquire))
            {
                return;
            }

            ctx->expressionsCompiled = true;
        }
        ctx->state.store(PresetSwitchState::GlPhases, std::memory_order_release);
    }
    catch (const std::exception& ex)
    {
        LOG_ERROR("[PresetCpuWorker] Expression compilation failed: " + std::string(ex.what()));
        ctx->errorMessage = ex.what();
        ctx->state.store(PresetSwitchState::Failed, std::memory_order_release);
    }
    catch (...)
    {
        LOG_ERROR("[PresetCpuWorker] Unknown exception during expression compilation.");
        ctx->errorMessage = "Unknown exception during expression compilation.";
        ctx->state.store(PresetSwitchState::Failed, std::memory_order_release);
    }
}

void PresetCpuWorker::DoFileRead(std::shared_ptr<PresetSwitchContext>& ctx)
{
    if (!ctx || ctx->cancelled.load(std::memory_order_acquire))
    {
        return;
    }

    try
    {
        std::string resolvedPath;
        std::string protocol = PresetFactory::Protocol(ctx->path, resolvedPath);

        if (!protocol.empty() && protocol != "file")
        {
            ctx->state.store(PresetSwitchState::GlStaging, std::memory_order_release);
            return;
        }

        if (ctx->cancelled.load(std::memory_order_acquire))
        {
            return;
        }

        std::ifstream file(resolvedPath.c_str(),
                           std::ios_base::in | std::ios_base::binary);
        if (!file.good())
        {
            ctx->errorMessage = "Could not open preset file: \"" + resolvedPath + "\".";
            ctx->state.store(PresetSwitchState::Failed, std::memory_order_release);
            return;
        }

        file.seekg(0, std::ios::end);
        auto size = file.tellg();
        file.seekg(0, std::ios::beg);

        if (size <= 0 || static_cast<size_t>(size) > 0x100000)
        {
            ctx->errorMessage = "Preset file has invalid size: \"" + resolvedPath + "\".";
            ctx->state.store(PresetSwitchState::Failed, std::memory_order_release);
            return;
        }

        std::string data(static_cast<size_t>(size), '\0');
        file.read(&data[0], size);

        if (file.fail() || file.bad())
        {
            ctx->errorMessage = "Failed to read preset file: \"" + resolvedPath + "\".";
            ctx->state.store(PresetSwitchState::Failed, std::memory_order_release);
            return;
        }

        if (ctx->cancelled.load(std::memory_order_acquire))
        {
            return;
        }

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

// ---- Platform-specific: threaded or synchronous ----

#if PROJECTM_USE_WORKER_THREAD

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
        if (m_expressionCtx)
        {
            m_expressionCtx->cancelled.store(true, std::memory_order_release);
        }
        // Also cancel in-flight work that the thread is currently
        // executing (these are copies held by ThreadLoop).
        if (m_activeFileCtx)
        {
            m_activeFileCtx->cancelled.store(true, std::memory_order_release);
        }
        if (m_activeExprCtx)
        {
            m_activeExprCtx->cancelled.store(true, std::memory_order_release);
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
        if (m_expressionCtx)
        {
            m_expressionCtx->cancelled.store(true, std::memory_order_release);
            m_expressionCtx.reset();
        }
        m_pending = std::move(ctx);
    }
    m_cv.notify_one();
}

void PresetCpuWorker::SubmitExpressionCompile(std::shared_ptr<PresetSwitchContext> ctx)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_expressionCtx)
        {
            m_expressionCtx->cancelled.store(true, std::memory_order_release);
        }
        m_expressionCtx = std::move(ctx);
    }
    m_cv.notify_one();
}

void PresetCpuWorker::ThreadLoop()
{
    while (true)
    {
        std::shared_ptr<PresetSwitchContext> ctx;
        std::shared_ptr<PresetSwitchContext> exprCtx;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return !m_running || m_pending || m_expressionCtx; });

            if (!m_running)
            {
                return;
            }

            ctx = std::move(m_pending);
            exprCtx = std::move(m_expressionCtx);

            // Store a reference so the destructor can cancel
            // in-flight work (the local copies above aren't
            // reachable from the destructor otherwise).
            m_activeFileCtx = ctx;
            m_activeExprCtx = exprCtx;
        }

        DoExpressionCompile(exprCtx);
        DoFileRead(ctx);

        // Clear active references under the lock.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_activeFileCtx.reset();
            m_activeExprCtx.reset();
        }
    }
}

#else // !PROJECTM_USE_WORKER_THREAD — synchronous fallback

PresetCpuWorker::PresetCpuWorker() = default;
PresetCpuWorker::~PresetCpuWorker() = default;

void PresetCpuWorker::StartLoad(std::shared_ptr<PresetSwitchContext> ctx)
{
    DoFileRead(ctx);
}

void PresetCpuWorker::SubmitExpressionCompile(std::shared_ptr<PresetSwitchContext> ctx)
{
    DoExpressionCompile(ctx);
}

#endif // PROJECTM_USE_WORKER_THREAD

} // namespace libprojectM
