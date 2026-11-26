#include <huxerui/app.h>

#include <iostream>

#include "single_instance.h"

int main() {
  const auto result = agentkit_preview::single_instance::StartOrActivate();
  if (result.status == agentkit_preview::single_instance::StartStatus::ActivatedExisting) {
    return 0;
  }
  if (!result.ShouldContinue()) {
    std::cerr << "single-instance startup failed: " << result.error << '\n';
    return 1;
  }
  agentkit_preview::single_instance::Guard instance_guard;
  return huxerui::RunApplication();
}
