#pragma once

#include "../cli_utils.h"

namespace CLI {
class App;
}

namespace rexglue::cli {

void RegisterPatches(CLI::App& parent, const CliContext& ctx, DeferredAction& pending);

}  // namespace rexglue::cli
