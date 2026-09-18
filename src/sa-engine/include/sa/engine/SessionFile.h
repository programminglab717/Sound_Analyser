#pragma once

#include <sa/core/Result.h>
#include <sa/engine/Document.h>
#include <sa/io/AudioSource.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace sa::engine {

/// Current session schema version.
///
/// Bumped whenever the on-disk shape changes. A reader refuses a version it
/// does not know rather than guessing: silently ignoring fields it cannot
/// understand would load a session that looks right and is not.
inline constexpr int kSessionVersion = 1;

/// Opens the audio a session references.
///
/// An interface rather than a direct call to io::openAudioFile so that tests
/// can load sessions without touching the filesystem, and so a future library
/// or cache can satisfy a reference without the loader knowing.
class SourceResolver {
public:
    virtual ~SourceResolver() = default;
    [[nodiscard]] virtual Result<std::shared_ptr<const io::AudioSource>>
    open(const std::filesystem::path& path) = 0;
};

/// Resolves through the filesystem.
class FileSourceResolver final : public SourceResolver {
public:
    [[nodiscard]] Result<std::shared_ptr<const io::AudioSource>>
    open(const std::filesystem::path& path) override;
};

/// A source the session referenced but the resolver could not open.
struct MissingSource {
    SourceId id = SourceId::Invalid;
    std::filesystem::path path;
    std::string reason;
};

struct SessionLoadResult {
    Document document;

    /// Sources that could not be opened.
    ///
    /// Their clips are still in the timeline, backed by a silent placeholder of
    /// the recorded length, so the arrangement survives intact and the UI can
    /// offer to relink. A session must not be destroyed because one file moved.
    std::vector<MissingSource> missingSources;

    /// Entries dropped during load because they were structurally invalid --
    /// a clip naming a source that is not in the file, a negative length. Each
    /// is described so a user learns what was lost rather than silently getting
    /// a different arrangement.
    std::vector<std::string> warnings;
};

/// Write `document` to `path` as JSON.
///
/// Source paths are stored relative to the session file where possible, so a
/// project folder can be moved or handed to someone else and still open.
[[nodiscard]] Status saveSession(const Document& document, const std::filesystem::path& path);

[[nodiscard]] Result<SessionLoadResult> loadSession(const std::filesystem::path& path,
                                                    SourceResolver& resolver);

/// Load using the filesystem resolver.
[[nodiscard]] Result<SessionLoadResult> loadSession(const std::filesystem::path& path);

/// Serialise to a JSON string, for tests and for embedding a session elsewhere.
[[nodiscard]] Result<std::string> sessionToJson(const Document& document,
                                                const std::filesystem::path& baseDirectory = {});

[[nodiscard]] Result<SessionLoadResult>
sessionFromJson(std::string_view json, SourceResolver& resolver,
                const std::filesystem::path& baseDirectory = {});

} // namespace sa::engine
