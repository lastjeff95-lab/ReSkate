#include "skater_safety.h"
#include "Extension/Console/commands.h"

// The skater safety console verb (skater_safety.h).
namespace dingosdk::console {
void register_skater_safety_commands(Commands &registry) {
    std::vector<Argument> arguments;
    auto rest = argument("boost|check|loop on|off", Type::text, true);
    rest.rest = true;
    arguments.push_back(std::move(rest));
    auto entry = action("safety", "Skater safety: boost limits, the physics check and the respawn loop breaker (kept)",
                        Group::gameplay, std::move(arguments));
    entry.run = [](const Model &, const Values &values, const Output &out) {
        std::vector<std::string> split;
        if (!values.empty())
            if (const auto *text = std::get_if<std::string>(&values[0])) {
                std::size_t start = 0;
                while (start < text->size()) {
                    auto end = text->find(' ', start);
                    if (end == std::string::npos) end = text->size();
                    if (end > start) split.push_back(text->substr(start, end - start));
                    start = end + 1;
                }
            }
        out(skater_safety::command(split));
    };
    registry.add(std::move(entry));
}
} // namespace dingosdk::console
