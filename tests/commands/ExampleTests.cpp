// SPDX-License-Identifier: GPL-3.0-or-later
#include <fstream>

#include "m2c/commands/Command.h"

#include "TestMeshes.h"

using namespace m2c;

// The files in examples/ are used by IDE run configurations and the README; keep them working.
TEST_CASE("The example batch session runs", "[commands][examples]") {
    CommandRegistry registry;
    REQUIRE(registerBuiltinCommands(registry).has_value());
    Document document;
    CommandContext context{document};

    const std::filesystem::path dir = M2C_EXAMPLES_DIR;
    std::ifstream session(dir / "session.jsonl");
    REQUIRE(session.good());

    std::string line;
    int executed = 0;
    while (std::getline(session, line)) {
        if (line.empty()) continue;
        const Json request = Json::parse(line);
        // Paths in the session are relative to examples/, as when run from the IDE.
        Json params = request.value("params", Json::object());
        if (params.contains("path"))
            params["path"] = (dir / params["path"].get<std::string>()).string();

        const auto result =
            registry.execute(context, request.at("command").get<std::string>(), params);
        INFO(line);
        REQUIRE(result.has_value());
        if (request.at("command") == "mesh.info") {
            CHECK((*result)["stats"]["is_watertight"] == true);
            CHECK((*result)["stats"]["face_count"] == 12);
        }
        ++executed;
    }
    CHECK(executed == 5);
    CHECK(document.objects().empty()); // the session ends with an undo
}
