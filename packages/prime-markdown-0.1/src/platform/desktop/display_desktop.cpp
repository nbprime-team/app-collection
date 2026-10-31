#include "display_desktop.h"

#include <cstdio>

namespace nmarkdown {
namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 240;

}  // namespace

DisplayDesktop::DisplayDesktop(std::string output_path)
    : output_path_(std::move(output_path)) {}

bool DisplayDesktop::initialize() {
    framebuffer_.assign(kWidth * kHeight, 0);
    initialized_ = true;
    write_succeeded_ = true;
    return true;
}

void DisplayDesktop::shutdown() {
    initialized_ = false;
}

Surface565 DisplayDesktop::surface() {
    if (!initialized_ || framebuffer_.empty()) {
        return {};
    }
    return {framebuffer_.data(), kWidth, kHeight, kWidth};
}

void DisplayDesktop::present() {
    if (!initialized_) {
        write_succeeded_ = false;
        return;
    }

    FILE* file = std::fopen(output_path_.c_str(), "wb");
    if (file == nullptr) {
        write_succeeded_ = false;
        return;
    }

    std::fprintf(file, "P6\n%d %d\n255\n", kWidth, kHeight);
    for (std::uint16_t color : framebuffer_) {
        const std::uint8_t bytes[3] = {red8(color), green8(color), blue8(color)};
        if (std::fwrite(bytes, 1, sizeof(bytes), file) != sizeof(bytes)) {
            write_succeeded_ = false;
            break;
        }
    }

    if (std::fclose(file) != 0) {
        write_succeeded_ = false;
    }
    ++present_count_;
}

}  // namespace nmarkdown

