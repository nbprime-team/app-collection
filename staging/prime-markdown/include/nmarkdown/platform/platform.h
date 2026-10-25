#ifndef NMARKDOWN_PLATFORM_PLATFORM_H
#define NMARKDOWN_PLATFORM_PLATFORM_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "nmarkdown/io/random_access.h"
#include "nmarkdown/render/surface565.h"

namespace nmarkdown {

enum class InputEventType : std::uint8_t {
    None,
    ScrollLineUp,
    ScrollLineDown,
    PageUp,
    PageDown,
    SwipeLeft,
    SwipeRight,
    SwipeUp,
    SwipeDown,
    PanLeft,
    PanRight,
    Activate,
    Back,
    Quit,
    OpenMenu,
    OpenSearch,
    OpenSettings,
    OpenDiagnostics,
    OpenDocument,
    SearchNext,
    ToggleBookmark,
    OpenBookmarks,
    TextInput,
    Backspace,
    IncreaseFont,
    DecreaseFont,
    PointerScroll,
    PointerPan,
};

enum class InputEventOrigin : std::uint8_t {
    Semantic,
    NumericNavigationAlias,
    TouchpadTap,
    TouchpadActivation,
};

struct InputEvent {
    InputEventType type = InputEventType::None;
    int amount = 0;
    InputEventOrigin origin = InputEventOrigin::Semantic;
};

struct DocumentProbe {
    std::uint64_t size = 0;
    std::uint32_t sample_hash = 2166136261U;
    std::uint32_t bytes_sampled = 0;
    bool sample_truncated = false;
};

class Display {
public:
    virtual ~Display() = default;
    virtual bool initialize() = 0;
    virtual void shutdown() = 0;
    virtual Surface565 surface() = 0;
    virtual void present() = 0;
};

class Input {
public:
    virtual ~Input() = default;
    virtual bool poll(InputEvent& event) = 0;
    // A poll can legitimately produce no semantic event while a key is held
    // or a touch gesture remains inside its deadzone. Background I/O and
    // shaping must still yield until that physical interaction ends.
    virtual bool interaction_active() const { return false; }
};

class FileSystem {
public:
    using OperationProgressCallback = void (*)(void* context,
                                               std::uint64_t completed,
                                               std::uint64_t total);

    virtual ~FileSystem() = default;
    // Synchronous adapters call report_operation_progress() between bounded
    // chunks of filesystem work. The application uses this hook only to make
    // delayed loading feedback visible; it does not cancel or mutate the
    // operation.
    void set_operation_progress_callback(OperationProgressCallback callback,
                                         void* context) {
        operation_progress_callback_ = callback;
        operation_progress_context_ = context;
    }
    virtual bool probe(const char* path,
                       DocumentProbe& result,
                       std::string& error) = 0;
    virtual bool read_all(const char* path,
                          std::size_t maximum_size,
                          std::vector<std::uint8_t>& data,
                          std::string& error) = 0;
    // TXT callers can transfer an already-owned UTF-8 string directly into
    // the document backing store. The default keeps compatibility with small
    // test adapters; StdioFileSystem overrides it to avoid a second complete
    // document allocation.
    virtual bool read_all_text(const char* path,
                               std::size_t maximum_size,
                               std::string& data,
                               std::string& error) {
        std::vector<std::uint8_t> bytes;
        if (!read_all(path, maximum_size, bytes, error)) {
            data.clear();
            return false;
        }
        if (bytes.empty()) {
            data.clear();
        } else {
            data.assign(reinterpret_cast<const char*>(bytes.data()),
                        bytes.size());
        }
        return true;
    }
    virtual bool read_range(const char* path,
                            std::uint64_t offset,
                            std::uint8_t* data,
                            std::size_t size,
                            std::string& error) = 0;
    // False with an empty error means persistent random access is unsupported
    // and the caller may use a bounded compatibility fallback. A non-empty
    // error is a real open failure.
    virtual bool open_random_access(
        const char* path,
        std::shared_ptr<RandomAccessData>& source,
        std::string& error) {
        (void)path;
        source.reset();
        error.clear();
        return false;
    }
    virtual bool write_atomic(const char* path,
                              const std::uint8_t* data,
                              std::size_t size,
                              std::string& error) = 0;
    virtual bool list_reader_documents(const char* root,
                                       std::size_t maximum_results,
                                       std::vector<std::string>& paths,
                                       std::string& error,
                                       bool* truncated = nullptr) {
        (void)root;
        (void)maximum_results;
        paths.clear();
        if (truncated != nullptr) *truncated = false;
        error = "document listing is not supported by this filesystem";
        return false;
    }
    virtual bool list_font_files(const char* root,
                                 std::size_t maximum_results,
                                 std::vector<std::string>& paths,
                                 std::string& error,
                                 bool* truncated = nullptr) {
        (void)root;
        (void)maximum_results;
        paths.clear();
        if (truncated != nullptr) *truncated = false;
        error = "font listing is not supported by this filesystem";
        return false;
    }

protected:
    void report_operation_progress(std::uint64_t completed = 0,
                                   std::uint64_t total = 0) const {
        if (operation_progress_callback_ != nullptr) {
            operation_progress_callback_(
                operation_progress_context_, completed, total);
        }
    }

private:
    OperationProgressCallback operation_progress_callback_ = nullptr;
    void* operation_progress_context_ = nullptr;
};

class Clock {
public:
    virtual ~Clock() = default;
    virtual std::uint64_t milliseconds() const = 0;
    virtual void sleep_ms(std::uint32_t duration) = 0;
};

class StdioFileSystem final : public FileSystem {
public:
    bool probe(const char* path,
               DocumentProbe& result,
               std::string& error) override;
    bool read_all(const char* path,
                  std::size_t maximum_size,
                  std::vector<std::uint8_t>& data,
                  std::string& error) override;
    bool read_all_text(const char* path,
                       std::size_t maximum_size,
                       std::string& data,
                       std::string& error) override;
    bool read_range(const char* path,
                    std::uint64_t offset,
                    std::uint8_t* data,
                    std::size_t size,
                    std::string& error) override;
    bool open_random_access(const char* path,
                            std::shared_ptr<RandomAccessData>& source,
                            std::string& error) override;
    bool write_atomic(const char* path,
                      const std::uint8_t* data,
                      std::size_t size,
                      std::string& error) override;
    bool list_reader_documents(const char* root,
                               std::size_t maximum_results,
                               std::vector<std::string>& paths,
                               std::string& error,
                               bool* truncated = nullptr) override;
    bool list_font_files(const char* root,
                         std::size_t maximum_results,
                         std::vector<std::string>& paths,
                         std::string& error,
                         bool* truncated = nullptr) override;
};

}  // namespace nmarkdown

#endif
