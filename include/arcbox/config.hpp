#pragma once

#include <filesystem>
#include <string>

namespace arcbox {

struct Config {
    std::filesystem::path data_dir;
    std::string user_agent =
        "ArchiveBox.cpp/0.1 (+https://github.com/MaurerAnton/archivebox-cpp)";
    long max_body_bytes = 100LL * 1024 * 1024;
    long connect_timeout_seconds = 15;
    long timeout_seconds = 60;

    // PSI-aware admission control.
    long min_mem_available_kb = 1500LL * 1024;
    double psi_some_avg10_max = 10.0;
    long throttle_sleep_seconds = 15;
    long pause_sleep_seconds = 60;

    // Queue retry policy. A failed capture is retried with exponential
    // backoff instead of being dropped on the first error.
    int max_attempts = 5;
    long retry_backoff_seconds = 30;
    long retry_backoff_max_seconds = 3600;

    bool use_cgroup = true;
    std::string slice;
    std::string memory_max = "1800M";
    std::string memory_high = "1200M";
    std::string memory_swap_max = "512M";
    std::string cpu_quota = "200%";
    int tasks_max = 300;

    static Config load();
    void ensure_dirs() const;

    std::filesystem::path db_path() const;
    std::filesystem::path warc_dir() const;
    std::filesystem::path snapshots_dir() const;
    std::filesystem::path objects_dir() const;
    std::filesystem::path blocklist_path() const;
    std::filesystem::path resolve(const std::string& relative) const;
};

} // namespace arcbox
