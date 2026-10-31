#ifndef NMARKDOWN_PLATFORM_DESKTOP_DISPLAY_DESKTOP_H
#define NMARKDOWN_PLATFORM_DESKTOP_DISPLAY_DESKTOP_H

#include <cstdint>
#include <string>
#include <vector>

#include "nmarkdown/platform/platform.h"

namespace nmarkdown {

class DisplayDesktop final : public Display {
public:
    explicit DisplayDesktop(std::string output_path);

    bool initialize() override;
    void shutdown() override;
    Surface565 surface() override;
    void present() override;

    int present_count() const { return present_count_; }
    bool write_succeeded() const { return write_succeeded_; }
    const std::string& output_path() const { return output_path_; }

private:
    std::string output_path_;
    std::vector<std::uint16_t> framebuffer_;
    int present_count_ = 0;
    bool initialized_ = false;
    bool write_succeeded_ = true;
};

}  // namespace nmarkdown

#endif

