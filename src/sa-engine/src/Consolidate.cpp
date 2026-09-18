#include <sa/engine/Consolidate.h>
#include <sa/io/WavWriter.h>

#include <algorithm>
#include <fstream>

namespace sa::engine {

namespace {

/// Blockwise, so consolidating a long recording costs one block rather than all
/// of it.
constexpr SampleCount kBlock = 65536;

[[nodiscard]] std::filesystem::path mediaFolder(const std::filesystem::path& sessionPath) {
    std::filesystem::path folder = sessionPath;
    folder.replace_extension();
    folder += ".media";
    return folder;
}

} // namespace

ConsolidateResult consolidateSources(Document& document, const std::filesystem::path& sessionPath) {
    ConsolidateResult result;

    std::vector<SourceId> pathless;
    for (const SourceEntry& entry : document.sources()) {
        if (entry.path.empty() && entry.audio) {
            pathless.push_back(entry.id);
        }
    }
    if (pathless.empty()) {
        return result;
    }

    const std::filesystem::path folder = mediaFolder(sessionPath);
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) {
        result.failures.push_back("could not create " + folder.string() + ": " + error.message());
        return result;
    }

    for (const SourceId id : pathless) {
        const SourceEntry* entry = document.source(id);
        if (entry == nullptr || !entry->audio) {
            continue;
        }
        const io::AudioFileInfo& info = entry->audio->info();

        // Named by id, which is unique within the document and stable across a
        // save. A name derived from the source's own label would collide the
        // moment two pastes came from the same place.
        const std::filesystem::path target =
            folder / ("source-" + std::to_string(static_cast<std::uint64_t>(id)) + ".wav");

        std::ofstream stream{target, std::ios::binary};
        if (!stream) {
            result.failures.push_back("could not write " + target.string());
            continue;
        }

        // Float32, not the 24-bit export default: this is intermediate audio
        // inside a project, and quantising a user's work every time they save
        // is a loss they never asked for and cannot undo.
        io::WavOptions options;
        options.format = io::SampleFormat::Float32;

        auto writer = io::WavWriter::create(stream, info.sampleRate, info.layout, options);
        if (!writer) {
            result.failures.push_back(target.string() + ": " + std::string{writer.error().what()});
            continue;
        }

        AudioBuffer block{info.layout, kBlock};
        SampleIndex cursor = 0;
        bool ok = true;
        while (cursor < info.frameCount) {
            const SampleCount want = std::min<SampleCount>(kBlock, info.frameCount - cursor);
            AudioBufferView view = block.view().subRange(0, want);
            const auto read = entry->audio->read(cursor, view);
            if (!read || read.value() <= 0) {
                break;
            }
            if (!writer.value().write(view.subRange(0, read.value()))) {
                result.failures.push_back(target.string() + ": write failed partway through");
                ok = false;
                break;
            }
            cursor += read.value();
        }
        if (!ok) {
            continue;
        }
        if (!writer.value().finish()) {
            result.failures.push_back(target.string() + ": could not finish the file");
            continue;
        }

        document.setSourcePath(id, target);
        result.written.push_back(target);
    }
    return result;
}

} // namespace sa::engine
