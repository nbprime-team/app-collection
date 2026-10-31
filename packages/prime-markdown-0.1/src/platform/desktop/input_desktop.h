#ifndef NMARKDOWN_PLATFORM_DESKTOP_INPUT_DESKTOP_H
#define NMARKDOWN_PLATFORM_DESKTOP_INPUT_DESKTOP_H

#include <cstddef>
#include <vector>

#include "nmarkdown/platform/platform.h"

namespace nmarkdown {

class ScriptedInput final : public Input {
public:
    explicit ScriptedInput(std::vector<InputEvent> events);
    bool poll(InputEvent& event) override;

private:
    std::vector<InputEvent> events_;
    std::size_t next_event_ = 0;
};

bool parse_input_event(const std::string& name, InputEvent& event);

}  // namespace nmarkdown

#endif

