#ifndef NMARKDOWN_PLATFORM_NDLESS_DISPLAY_NDLESS_H
#define NMARKDOWN_PLATFORM_NDLESS_DISPLAY_NDLESS_H

#include <cstdint>
#include <vector>

#include "nmarkdown/platform/platform.h"

namespace nmarkdown {

class DisplayNdless final : public Display {
public:
    bool initialize() override;
    void shutdown() override;
    Surface565 surface() override;
    void present() override;

private:
    std::vector<std::uint16_t> framebuffer_;
    std::vector<std::uint16_t> native_blit_source_;
    bool native_240x320_ = false;
    bool initialized_ = false;
};

}  // namespace nmarkdown

#endif
