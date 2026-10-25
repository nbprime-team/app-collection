#include "display_ndless.h"

#include <libndls.h>

#include "nmarkdown/platform/ndless/display_sync.h"

namespace nmarkdown {
namespace {

constexpr int kWidth = display_sync::kLandscapeWidth;
constexpr int kHeight = display_sync::kLandscapeHeight;
constexpr std::size_t kPixelCount =
    static_cast<std::size_t>(kWidth) * kHeight;

// PL110/PL111 LCD interrupt bit 2 is vertical compare. Bit 3 is the AHB
// master-error interrupt; waiting on bit 3 was the bug in the older
// synchronization attempt.
constexpr std::uint32_t kLcdVerticalCompare = 0x04U;
constexpr std::uint32_t kCxMaximumVsyncPolls = 800000U;
constexpr std::uint32_t kCx2MaximumVsyncPolls = 2400000U;

void clean_dcache_range(void* data, std::size_t size) {
    if (data == nullptr || size == 0) return;
    constexpr std::uintptr_t kCacheLineBytes = 32;
    std::uintptr_t current =
        reinterpret_cast<std::uintptr_t>(data) & ~(kCacheLineBytes - 1U);
    const std::uintptr_t end =
        (reinterpret_cast<std::uintptr_t>(data) + size +
         kCacheLineBytes - 1U) &
        ~(kCacheLineBytes - 1U);
    for (; current < end; current += kCacheLineBytes) {
        __asm volatile(
            "mcr p15, 0, %0, c7, c10, 1"
            :
            : "r"(current)
            : "memory");
    }
    const std::uint32_t zero = 0;
    __asm volatile(
        "mcr p15, 0, %0, c7, c10, 4"
        :
        : "r"(zero)
        : "memory");
}

bool wait_for_next_vertical_compare() {
    volatile std::uint32_t* const raw_interrupt =
        reinterpret_cast<volatile std::uint32_t*>(0xC0000020U);
    volatile std::uint32_t* const interrupt_clear =
        reinterpret_cast<volatile std::uint32_t*>(0xC0000028U);
    const std::uint32_t maximum_polls =
        is_cx2 ? kCx2MaximumVsyncPolls : kCxMaximumVsyncPolls;

    const bool initially_asserted =
        (*raw_interrupt & kLcdVerticalCompare) != 0;
    display_sync::VerticalCompareEdge edge(!initially_asserted);

    // Discard any old latched event, then require a new assertion. In
    // particular, do not accept a bit that remains asserted while the clear
    // reaches the controller as the next display boundary.
    *interrupt_clear = kLcdVerticalCompare;
    for (std::uint32_t poll = 0; poll < maximum_polls; ++poll) {
        const bool asserted =
            (*raw_interrupt & kLcdVerticalCompare) != 0;
        const display_sync::VerticalCompareAction action =
            edge.observe(asserted);
        if (action ==
            display_sync::VerticalCompareAction::AcknowledgeCurrent) {
            *interrupt_clear = kLcdVerticalCompare;
        } else if (action ==
                   display_sync::VerticalCompareAction::Present) {
            *interrupt_clear = kLcdVerticalCompare;
            return true;
        }
    }
    // Keep presentation live if an emulator or unexpected controller does not
    // expose the vertical-compare interrupt.
    return false;
}

}  // namespace

bool DisplayNdless::initialize() {
    // Always expose the renderer's stable landscape RGB565 format. HW-W/CX II
    // calculators scan out 240x320, so they also receive a native-layout
    // staging buffer. It remains an lcd_blit source; unlike the removed direct
    // page-flip experiment, it is never installed as the PL111 DMA address.
    if (!lcd_init(SCR_320x240_565)) {
        return false;
    }
    const scr_type_t physical_type = lcd_type();
    native_240x320_ = physical_type == SCR_240x320_565 ||
                      physical_type == SCR_240x320_555;
    try {
        framebuffer_.assign(kPixelCount, 0);
        if (native_240x320_) {
            native_blit_source_.assign(kPixelCount, 0);
        } else {
            native_blit_source_.clear();
        }
    } catch (...) {
        framebuffer_.clear();
        native_blit_source_.clear();
    }
    initialized_ = !framebuffer_.empty() &&
                   (!native_240x320_ || !native_blit_source_.empty());
    if (!initialized_) {
        lcd_init(SCR_TYPE_INVALID);
        native_240x320_ = false;
    }
    return initialized_;
}

void DisplayNdless::shutdown() {
    if (!initialized_) return;
    lcd_init(SCR_TYPE_INVALID);
    native_240x320_ = false;
    initialized_ = false;
}

Surface565 DisplayNdless::surface() {
    if (!initialized_ || framebuffer_.empty()) {
        return {};
    }
    return {framebuffer_.data(), kWidth, kHeight, kWidth};
}

void DisplayNdless::present() {
    if (!initialized_) return;

    void* blit_source = framebuffer_.data();
    scr_type_t blit_type = SCR_320x240_565;
    if (native_240x320_) {
        // Ndless's landscape HW-W blit transposes while writing the active LCD
        // buffer. That long, strided copy can run past blanking and tear. Do
        // the rotation first so the post-Vsync syscall is a short contiguous
        // memcpy into the OS-owned scanout.
        display_sync::stage_landscape_rgb565_for_native(
            framebuffer_.data(), native_blit_source_.data());
        blit_source = native_blit_source_.data();
        blit_type = SCR_240x320_565;
    }

    // Clean the exact source after all staging writes, then start the supported
    // Ndless copy on a fresh vertical-compare edge. lcd_blit retains TI-OS
    // address handling; nMarkdown never points PL111 at heap storage.
    clean_dcache_range(blit_source,
                       kPixelCount * sizeof(std::uint16_t));
    wait_for_next_vertical_compare();
    lcd_blit(blit_source, blit_type);
}

}  // namespace nmarkdown
