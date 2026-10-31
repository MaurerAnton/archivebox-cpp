#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "arcbox/config.hpp"

namespace arcbox {

int cmd_init(Config& cfg);
int cmd_add(Config& cfg,
            const std::string& url,
            bool capture_now,
            bool use_blocklist);
int cmd_capture(Config& cfg,
                const std::string& target,
                bool use_cgroup,
                bool use_blocklist);
int cmd_list(Config& cfg, int limit, const std::string& tag = {});
int cmd_search(Config& cfg,
               const std::string& query,
               int limit,
               const std::string& tag = {});
int cmd_show(Config& cfg, int64_t id);
int cmd_tag(Config& cfg,
            int64_t id,
            const std::vector<std::string>& tags,
            bool remove);
int cmd_tags(Config& cfg);
int cmd_import(Config& cfg,
               const std::string& path,
               bool capture_now,
               bool use_cgroup,
               bool use_blocklist);
int cmd_export(Config& cfg, int limit, const std::string& tag, bool urls_only);
int cmd_daemon(Config& cfg,
               bool once,
               bool use_cgroup,
               bool use_blocklist);
int cmd_worker(Config& cfg, int64_t id, bool use_blocklist);
int cmd_serve(Config& cfg, const std::string& host, int port);

} // namespace arcbox
