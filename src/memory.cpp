#include "arcbox/memory.hpp"

#include <fstream>
#include <sstream>
#include <string>

namespace arcbox {

namespace {

bool parse_kb_value(const std::string& line, uint64_t& out) {
    auto colon = line.find(':');
    if (colon == std::string::npos) {
        return false;
    }
    std::istringstream iss(line.substr(colon + 1));
    uint64_t value = 0;
    if (!(iss >> value)) {
        return false;
    }
    out = value;
    return true;
}

bool parse_psi_avg10(const std::string& line, const char* key, double& out) {
    std::string prefix(key);
    if (line.rfind(prefix, 0) != 0) {
        return false;
    }
    auto pos = line.find("avg10=");
    if (pos == std::string::npos) {
        return false;
    }
    try {
        out = std::stod(line.substr(pos + 6));
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace

bool read_host_memory(HostMemory& out) {
    out = HostMemory{};
    bool have_meminfo = false;

    std::ifstream meminfo("/proc/meminfo");
    if (meminfo) {
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.rfind("MemTotal:", 0) == 0) {
                if (parse_kb_value(line, out.mem_total_kb)) {
                    have_meminfo = true;
                }
            } else if (line.rfind("MemAvailable:", 0) == 0) {
                if (parse_kb_value(line, out.mem_available_kb)) {
                    have_meminfo = true;
                }
            }
        }
    }

    std::ifstream psi("/proc/pressure/memory");
    if (psi) {
        std::string line;
        while (std::getline(psi, line)) {
            parse_psi_avg10(line, "some ", out.psi_some_avg10);
            parse_psi_avg10(line, "full ", out.psi_full_avg10);
        }
        out.psi_supported = true;
    }

    return have_meminfo;
}

AdmissionDecision admit_capture(const Config& cfg, const HostMemory& mem) {
    AdmissionDecision decision;

    // A zero reading means /proc/meminfo was unreadable; fail open rather than
    // stalling the queue forever.
    if (mem.mem_available_kb > 0 &&
        static_cast<long>(mem.mem_available_kb) < cfg.min_mem_available_kb) {
        decision.action = Admission::Pause;
        decision.reason = "MemAvailable " +
                          std::to_string(mem.mem_available_kb / 1024) +
                          "M below floor " +
                          std::to_string(cfg.min_mem_available_kb / 1024) + "M";
        decision.sleep_seconds = cfg.pause_sleep_seconds;
        return decision;
    }

    if (mem.psi_supported && mem.psi_some_avg10 > cfg.psi_some_avg10_max) {
        decision.action = Admission::Throttle;
        decision.reason = "PSI memory some avg10 " +
                          std::to_string(mem.psi_some_avg10) + " above " +
                          std::to_string(cfg.psi_some_avg10_max);
        decision.sleep_seconds = cfg.throttle_sleep_seconds;
        return decision;
    }

    decision.action = Admission::Proceed;
    decision.reason = "host memory healthy";
    return decision;
}

const char* admission_name(Admission action) {
    switch (action) {
        case Admission::Proceed:
            return "proceed";
        case Admission::Throttle:
            return "throttle";
        case Admission::Pause:
            return "pause";
    }
    return "unknown";
}

} // namespace arcbox
