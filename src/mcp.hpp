#pragma once
#include "service.hpp"
#include <functional>
namespace mora {
    class McpServer {
      public:
        using Handler = std::function<Json(const std::string&, const Json&)>;
        explicit McpServer(Handler handler);
        std::optional<Json> handle(const Json& request);

      private:
        enum class Phase { AwaitInitialize, AwaitInitialized, Ready };
        Handler invoke;
        Phase phase = Phase::AwaitInitialize;
    };
    int runMcp();
} // namespace mora
