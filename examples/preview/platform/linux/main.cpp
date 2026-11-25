#include <huxerui/app.h>

#include "single_instance.h"

int main() {
  if (!agentkit_preview::single_instance::StartOrActivate()) {
    return 0;
  }
  agentkit_preview::single_instance::Guard instance_guard;
  return huxerui::RunApplication();
}
