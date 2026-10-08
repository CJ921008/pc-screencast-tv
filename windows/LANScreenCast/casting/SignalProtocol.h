#pragma once
#include <nlohmann/json.hpp>
#include <regex>
#include <stdexcept>
#include <set>
namespace LANScreenCast::casting {
inline void ValidateSignal(nlohmann::json const& message, std::string const& session) {
    static std::regex uuid("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
    static std::set<std::string> kinds{"hello","capabilities","offer","answer","ice_candidate","ping","pong","disconnect","error","stats"};
    if (!message.is_object() || message.size()!=5 || message.value("protocolVersion",0)!=2 ||
        !message.contains("timestamp") || !message["timestamp"].is_number_integer() || message["timestamp"].get<int64_t>()<0 ||
        !message.contains("requestId") || !message["requestId"].is_string() ||
        !std::regex_match(message["requestId"].get<std::string>(),uuid) ||
        !message.contains("type") || !message["type"].is_string() || !kinds.count(message["type"].get<std::string>()) ||
        !message.contains("payload") || !message["payload"].is_object())
        throw std::runtime_error("Invalid v2 signal envelope");
    auto const& payload=message["payload"];
    if (!payload.contains("sessionId") || !payload["sessionId"].is_string()) throw std::runtime_error("Missing signal session");
    std::string id=payload["sessionId"];
    if (message["type"]=="error" && id.empty()) return;
    if(id!=session || !std::regex_match(id,uuid)) throw std::runtime_error("Invalid signal session");
}
}
