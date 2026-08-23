#include "server_runtime.h"

namespace aqua::runtime {

// Intentionally empty for now. The application/runtime layer will later own
// orchestration of capture -> transport. Backend-specific realtime threads
// stay inside their backend implementation to preserve dependency direction.

} // namespace aqua::runtime
