/*
 Licensed under the Apache License, Version 2.0
*/
#pragma once
#include <unordered_map>
#include <memory>
#include <mutex>
#include <Identifiers/Identifiers.hpp>
#include <Runtime/Execution/OperatorHandler.hpp>

namespace NES {

class RecoveredOperatorHandlersRegistry {
public:
    using HandlerMap = std::unordered_map<OperatorHandlerId, std::shared_ptr<OperatorHandler>>;

    // Set a pending handler map to be associated with the next registered query
    static void setPending(HandlerMap map);

    // Consume pending and bind to a query id
    static void consumePendingForQuery(QueryId qid);

    // Retrieve bound map for a query id (may be empty)
    static HandlerMap getForQuery(QueryId qid);

    // Clear mapping for a query id
    static void clearForQuery(QueryId qid);

private:
    static std::mutex mtx;
    static inline HandlerMap pendingHandlers{};
    static inline std::unordered_map<QueryId, HandlerMap> registry;
};

} // namespace NES
