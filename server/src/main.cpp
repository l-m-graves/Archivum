// archivum: one binary. Stage 1 provides `archivum serve --config <path>`.
// Operational subcommands (check, dump, restore, backup) arrive with Stage 4.
#include <cstdio>
#include <cstring>
#include <string>

#include "archivum/server/app.h"

namespace {

int usage() {
  std::fprintf(stderr,
               "usage:\n"
               "  archivum serve --config <path>    run the application server\n"
               "  archivum version\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  const std::string command = argv[1];
  if (command == "version") {
    std::printf("archivum 0.0.1 (stage 1)\n");
    return 0;
  }
  if (command != "serve") return usage();
  std::string config_path;
  for (int i = 2; i < argc; ++i) {
    if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
      config_path = argv[++i];
    } else {
      return usage();
    }
  }
  if (config_path.empty()) return usage();

  auto config = archivum::server::load_config(config_path);
  if (!config.ok()) {
    std::fprintf(stderr, "configuration rejected: %s\n", config.status().to_string().c_str());
    return 1;
  }
  archivum::server::App app(std::move(config).value());
  if (archivum::Status s = app.configure(); !s.ok()) {
    std::fprintf(stderr, "startup failed: %s\n", s.to_string().c_str());
    return 1;
  }
  std::fprintf(stderr, "archivum listening on %s:%u (TLS)\n", app.config().listen_address.c_str(),
               static_cast<unsigned>(app.config().listen_port));
  archivum::Status s = app.run();
  if (!s.ok()) {
    std::fprintf(stderr, "server stopped: %s\n", s.to_string().c_str());
    return 1;
  }
  return 0;
}
