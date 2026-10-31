#include "arcbox/cgroup.hpp"

#include <string>
#include <vector>

#include <cstdlib>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace arcbox {

bool cgroup_available() {
    const char* disabled = std::getenv("ARCBOX_NO_CGROUP");
    if (disabled && std::string(disabled) == "1") {
        return false;
    }

    if (access("/usr/bin/systemd-run", X_OK) != 0 &&
        access("/bin/systemd-run", X_OK) != 0) {
        return false;
    }

    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (!runtime || !*runtime) {
        return false;
    }

    std::string bus = std::string(runtime) + "/bus";
    return access(bus.c_str(), F_OK) == 0;
}

int run_in_cgroup(const Config& cfg,
                  const std::vector<std::string>& argv,
                  std::string& error) {
    if (argv.empty()) {
        error = "empty command";
        return 127;
    }

    std::vector<std::string> args = {
        "systemd-run", "--user", "--scope", "--collect", "--quiet", "--same-dir"};
    if (!cfg.slice.empty()) {
        args.push_back("--slice=" + cfg.slice);
    }
    args.push_back("-p");
    args.push_back("MemoryHigh=" + cfg.memory_high);
    args.push_back("-p");
    args.push_back("MemoryMax=" + cfg.memory_max);
    args.push_back("-p");
    args.push_back("MemorySwapMax=" + cfg.memory_swap_max);
    args.push_back("-p");
    args.push_back("CPUQuota=" + cfg.cpu_quota);
    args.push_back("-p");
    args.push_back("TasksMax=" + std::to_string(cfg.tasks_max));
    args.push_back("--");
    for (const auto& arg : argv) {
        args.push_back(arg);
    }

    pid_t pid = fork();
    if (pid < 0) {
        error = "fork failed";
        return 127;
    }
    if (pid == 0) {
        std::vector<char*> raw;
        raw.reserve(args.size() + 1);
        for (auto& arg : args) {
            raw.push_back(const_cast<char*>(arg.c_str()));
        }
        raw.push_back(nullptr);
        execvp(raw[0], raw.data());
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        error = "waitpid failed";
        return 127;
    }
    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code != 0) {
            error = "systemd-run exited with status " + std::to_string(code);
        }
        return code;
    }
    if (WIFSIGNALED(status)) {
        error = "systemd-run killed by signal " +
                std::to_string(WTERMSIG(status));
        return 128 + WTERMSIG(status);
    }
    error = "systemd-run did not exit normally";
    return 127;
}

} // namespace arcbox
