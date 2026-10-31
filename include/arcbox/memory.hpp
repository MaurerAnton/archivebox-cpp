#pragma once

#include <cstdint>
#include <string>

#include "arcbox/config.hpp"

namespace arcbox {

struct HostMemory {
    uint64_t mem_total_kb = 0;
    uint64_t mem_available_kb = 0;
    double psi_some_avg10 = 0.0;
    double psi_full_avg10 = 0.0;
    bool psi_supported = false;
};

/// Sample host memory state from /proc/meminfo and /proc/pressure/memory.
/// psi_supported is false when CONFIG_PSI is unavailable; PSI fields stay 0.
bool read_host_memory(HostMemory& out);

enum class Admission {
    Proceed,
    Throttle,
    Pause,
};

struct AdmissionDecision {
    Admission action = Admission::Proceed;
    std::string reason;
    long sleep_seconds = 0;
};

/// Decide whether the queue may start the next capture.
AdmissionDecision admit_capture(const Config& cfg, const HostMemory& mem);

const char* admission_name(Admission action);

} // namespace arcbox
