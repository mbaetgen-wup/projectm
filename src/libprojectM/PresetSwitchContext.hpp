/**
 * @file PresetSwitchContext.hpp
 * @brief Holds all data associated with a single asynchronous preset switch.
 */
#pragma once

#include "PresetSwitchState.hpp"

#include <atomic>
#include <memory>
#include <string>

namespace libprojectM {

class Preset;

/**
 * @brief Represents a single in-flight preset transition.
 *
 * Thread-safety model:
 *  - @c cancelled and @c state are atomic and may be read/written from any thread.
 *  - @c path and @c smoothTransition are set once before submission and read-only after.
 *  - @c fileData and @c errorMessage are written only by the CPU worker before
 *    the state advances to GlStaging, and read only by the render thread afterwards.
 *  - @c preset and @c glInitialized are accessed exclusively on the render thread.
 *
 * Only one active context exists at a time.  When a new switch is requested the
 * previous context is cancelled via the atomic flag.
 */
struct PresetSwitchContext
{
    /// Set to true by any thread to abort this switch.
    std::atomic<bool> cancelled{false};

    /// Current state of this switch (atomic for cross-thread visibility).
    std::atomic<PresetSwitchState> state{PresetSwitchState::Idle};

    /// Path / URL of the preset to load.
    std::string path;

    /// True = soft transition, false = hard cut.
    bool smoothTransition{true};

    // ---- CPU-produced data (written by worker, read on render thread) ----

    /// Raw file contents read by the CPU worker.  Empty until CpuLoading
    /// completes successfully.  The render thread uses this to construct
    /// the preset via the factory stream interface.
    std::string fileData;

    /// Error message set by the CPU worker when loading fails.
    std::string errorMessage;

    // ---- GL staging data (render thread only) ----

    /// The fully-constructed and GL-initialized preset.
    /// Created and populated exclusively on the render thread.
    std::unique_ptr<Preset> preset;

    /// Tracks which GL initialization phase has been completed.
    /// 0 = not started, 1 = setup/expressions/framebuffers done,
    /// 2 = warp shader compiled, 3 = composite shader compiled (ready).
    int glInitPhase{0};

    /// True if the current glInitPhase has been executed but may still
    /// have async work in flight (e.g. shader compilation via
    /// GL_KHR_parallel_shader_compile).
    bool glInitPhaseExecuted{false};
};

} // namespace libprojectM
