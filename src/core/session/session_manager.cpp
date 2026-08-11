#include "core/session/session_manager.h"

namespace aqua {

SessionManager::SessionManager()
    : random_generator_(std::random_device{}())
    , uuid_generator_(random_generator_) {}

SessionManager::~SessionManager() = default;

SessionManager::session_id_t SessionManager::generate_session_id() {
    return uuid_generator_();
}

} // namespace aqua
