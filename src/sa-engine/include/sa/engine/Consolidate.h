#pragma once

#include <sa/core/Result.h>
#include <sa/engine/Document.h>

#include <filesystem>
#include <vector>

namespace sa::engine {

struct ConsolidateResult {
    /// Files written, in the order they were produced.
    std::vector<std::filesystem::path> written;

    /// Sources that could not be written, with the reason. Their clips stay in
    /// the timeline; the caller decides whether that is fatal.
    std::vector<std::string> failures;
};

/// Write every source with no file behind it beside `sessionPath`, and point
/// the document at what was written.
///
/// Editing generates audio that exists only in memory: a paste, a flattened
/// range, a spectral repair. A session records sources by path, so saving one
/// without doing this reopens with that audio replaced by silence and the
/// arrangement intact -- which looks like the session worked and did not.
///
/// Files land in a folder named after the session, so a project is one file
/// plus one folder and moving both keeps it working.
[[nodiscard]] ConsolidateResult consolidateSources(Document& document,
                                                   const std::filesystem::path& sessionPath);

} // namespace sa::engine
