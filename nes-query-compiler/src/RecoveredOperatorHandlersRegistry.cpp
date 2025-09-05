/*
 Licensed under the Apache License, Version 2.0
*/
#include <RecoveredOperatorHandlersRegistry.hpp>

namespace NES {

std::mutex RecoveredOperatorHandlersRegistry::mtx;

void RecoveredOperatorHandlersRegistry::setPending(HandlerMap map) {
    std::scoped_lock lk(mtx);
    pendingHandlers = std::move(map);
}

void RecoveredOperatorHandlersRegistry::consumePendingForQuery(QueryId qid) {
    std::scoped_lock lk(mtx);
    if (!pendingHandlers.empty()) {
        registry[qid] = pendingHandlers;
        pendingHandlers.clear();
    }
}

RecoveredOperatorHandlersRegistry::HandlerMap RecoveredOperatorHandlersRegistry::getForQuery(QueryId qid) {
    std::scoped_lock lk(mtx);
    if (auto it = registry.find(qid); it != registry.end()) {
        return it->second;
    }
    return {};
}

void RecoveredOperatorHandlersRegistry::clearForQuery(QueryId qid) {
    std::scoped_lock lk(mtx);
    registry.erase(qid);
}

} // namespace NES
