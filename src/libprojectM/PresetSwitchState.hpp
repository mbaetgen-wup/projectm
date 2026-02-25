/**
 * @file PresetSwitchState.hpp
 * @brief Enumerates the states of an asynchronous preset switch operation.
 */
#pragma once

#include <cstdint>

namespace libprojectM {

/**
 * @brief States of a single preset switch operation.
 *
 * The state machine progresses as follows:
 *   Idle -> CpuLoading -> GlStaging -> Activating -> Completed
 *
 * At any point before Completed the switch may transition to Failed.
 */
enum class PresetSwitchState : uint8_t
{
    Idle,        ///< Context has been created but work has not started.
    CpuLoading,  ///< CPU worker is reading the preset file from disk.
    GlStaging,   ///< File data is ready; render thread constructs preset and GL resources.
    Activating,  ///< All GL resources are ready; preset is being activated.
    Completed,   ///< The new preset is active.  Context may be discarded.
    Failed       ///< The switch failed (see errorMessage) or was cancelled.
};

} // namespace libprojectM
