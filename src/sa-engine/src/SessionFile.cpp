#include <sa/engine/SessionFile.h>
#include <sa/engine/SilentSource.h>
#include <sa/io/AudioFile.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <nlohmann/json.hpp>
#include <sstream>

namespace sa::engine {

namespace {

using Json = nlohmann::json;

constexpr const char* kFormatTag = "auscultate-session";

std::string_view fadeShapeName(FadeShape shape) noexcept {
    switch (shape) {
    case FadeShape::Linear:
        return "linear";
    case FadeShape::EqualPower:
        return "equalPower";
    case FadeShape::Logarithmic:
        return "logarithmic";
    case FadeShape::Exponential:
        return "exponential";
    case FadeShape::SCurve:
        return "sCurve";
    }
    return "equalPower";
}

FadeShape fadeShapeFromName(std::string_view name) noexcept {
    if (name == "linear")
        return FadeShape::Linear;
    if (name == "logarithmic")
        return FadeShape::Logarithmic;
    if (name == "exponential")
        return FadeShape::Exponential;
    if (name == "sCurve")
        return FadeShape::SCurve;
    return FadeShape::EqualPower;
}

Json fadeToJson(const Fade& fade) {
    return Json{{"length", fade.length}, {"shape", fadeShapeName(fade.shape)}};
}

Fade fadeFromJson(const Json& json) {
    Fade fade;
    if (json.is_object()) {
        fade.length = json.value("length", SampleCount{0});
        fade.shape = fadeShapeFromName(json.value("shape", std::string{"equalPower"}));
    }
    return std::max<SampleCount>(0, fade.length) == fade.length ? fade : Fade{};
}

/// Store a path relative to the session file when it is below it, so a project
/// folder can be moved or handed to someone else and still open. Paths outside
/// that subtree stay absolute, because a relative path climbing out of the
/// folder is more fragile than an absolute one, not less.
std::string encodePath(const std::filesystem::path& path,
                       const std::filesystem::path& baseDirectory) {
    if (baseDirectory.empty()) {
        return path.generic_string();
    }
    std::error_code error;
    const auto relative = std::filesystem::relative(path, baseDirectory, error);
    // Compare the first path component rather than the string. path::native()
    // is std::wstring on Windows, so a narrow literal does not convert there --
    // and a component comparison is more precise anyway: starts_with("..")
    // would also match a file genuinely named "..archive".
    const bool climbsOut = relative.begin() != relative.end() && *relative.begin() == "..";
    if (error || relative.empty() || climbsOut) {
        return path.generic_string();
    }
    return relative.generic_string();
}

std::filesystem::path decodePath(const std::string& text,
                                 const std::filesystem::path& baseDirectory) {
    std::filesystem::path path{text};
    if (path.is_absolute() || baseDirectory.empty()) {
        return path;
    }
    return baseDirectory / path;
}

/// Read a number that must be a non-negative integer, rejecting anything the
/// file claims that could not be true. Every value here is attacker-controlled
/// in exactly the way a WAV chunk size is.
bool readCount(const Json& json, const char* key, SampleCount& out) {
    if (!json.contains(key) || !json[key].is_number_integer()) {
        return false;
    }
    const auto value = json[key].get<std::int64_t>();
    if (value < 0) {
        return false;
    }
    out = value;
    return true;
}

} // namespace

Result<std::shared_ptr<const io::AudioSource>>
FileSourceResolver::open(const std::filesystem::path& path) {
    return io::openAudioFile(path);
}

Result<std::string> sessionToJson(const Document& document,
                                  const std::filesystem::path& baseDirectory) {
    Json root;
    root["format"] = kFormatTag;
    root["version"] = kSessionVersion;
    root["sampleRate"] = document.sampleRate().hz();
    root["channels"] = document.layout().count();
    root["nextId"] = document.nextId();

    // Sources are addressed by id, not array position, so the ordering here is
    // presentation only.
    Json sources = Json::array();
    for (const SourceEntry& entry : document.sources()) {
        Json source;
        source["id"] = static_cast<std::uint64_t>(entry.id);
        source["name"] = entry.name;
        source["path"] = encodePath(entry.path, baseDirectory);
        // The shape is recorded as well as the path so a missing file can be
        // replaced by a silent placeholder of the right length, keeping the
        // arrangement intact.
        if (entry.audio != nullptr) {
            const io::AudioFileInfo& info = entry.audio->info();
            source["frameCount"] = info.frameCount;
            source["sourceSampleRate"] = info.sampleRate.hz();
            source["sourceChannels"] = info.channelCount();
        }
        sources.push_back(std::move(source));
    }
    root["sources"] = std::move(sources);

    Json clips = Json::array();
    for (const Clip& clip : document.timeline().clips()) {
        Json item;
        item["id"] = static_cast<std::uint64_t>(clip.id);
        item["source"] = static_cast<std::uint64_t>(clip.source);
        item["sourceStart"] = clip.sourceStart;
        item["length"] = clip.length;
        item["timelineStart"] = clip.timelineStart;
        item["gain"] = clip.gain;
        item["fadeIn"] = fadeToJson(clip.fadeIn);
        item["fadeOut"] = fadeToJson(clip.fadeOut);
        item["name"] = clip.name;
        clips.push_back(std::move(item));
    }
    root["clips"] = std::move(clips);

    Json markers = Json::array();
    for (const Marker& marker : document.markers()) {
        markers.push_back(Json{
            {"position", marker.position}, {"length", marker.length}, {"label", marker.label}});
    }
    root["markers"] = std::move(markers);

    return root.dump(2);
}

Result<SessionLoadResult> sessionFromJson(std::string_view json, SourceResolver& resolver,
                                          const std::filesystem::path& baseDirectory) {
    Json root = Json::parse(json, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        return Error{ErrorCode::CorruptData, "session file is not valid JSON"};
    }

    if (root.value("format", std::string{}) != kFormatTag) {
        return Error{ErrorCode::UnsupportedFormat, "not an Auscultate session file"};
    }

    const int version = root.value("version", 0);
    if (version <= 0) {
        return Error{ErrorCode::CorruptData, "session file has no usable version"};
    }
    if (version > kSessionVersion) {
        // Refusing beats guessing: loading a newer file by ignoring the fields
        // we do not understand produces a session that looks right and is not,
        // and then overwrites the original on save.
        return Error{ErrorCode::UnsupportedFormat,
                     "session was written by a newer version (" + std::to_string(version) +
                         "; this build reads up to " + std::to_string(kSessionVersion) + ")"};
    }

    const double rate = root.value("sampleRate", 0.0);
    const SampleRate sampleRate{rate};
    if (!sampleRate.isValid()) {
        return Error{ErrorCode::CorruptData, "session has an invalid sample rate"};
    }

    const int channels = root.value("channels", 0);
    if (channels <= 0 || channels > kMaxChannels) {
        return Error{ErrorCode::CorruptData, "session has an invalid channel count"};
    }

    SessionLoadResult result;
    result.document = Document{sampleRate, channels == 1   ? ChannelLayout::mono()
                                           : channels == 2 ? ChannelLayout::stereo()
                                                           : ChannelLayout::discrete(channels)};

    // Ids are reassigned exactly as recorded, so clip references stay valid.
    std::map<std::uint64_t, SourceId> sourceIds;

    if (root.contains("sources") && root["sources"].is_array()) {
        for (const Json& item : root["sources"]) {
            if (!item.is_object() || !item.contains("id")) {
                result.warnings.emplace_back("dropped a source entry with no id");
                continue;
            }
            const auto rawId = item.value("id", std::uint64_t{0});
            if (rawId == 0) {
                result.warnings.emplace_back("dropped a source with id 0, which is reserved");
                continue;
            }

            const auto storedPath = item.value("path", std::string{});
            const auto resolved = decodePath(storedPath, baseDirectory);
            const auto name = item.value("name", std::string{});

            SampleCount frames = 0;
            readCount(item, "frameCount", frames);
            const double sourceRate = item.value("sourceSampleRate", rate);
            const int sourceChannels = item.value("sourceChannels", channels);

            std::shared_ptr<const io::AudioSource> audio;
            std::string failure;
            if (!storedPath.empty()) {
                auto opened = resolver.open(resolved);
                if (opened) {
                    audio = opened.value();
                } else {
                    failure = std::string{opened.error().what()};
                }
            } else {
                failure = "session recorded no path for this source";
            }

            if (audio == nullptr) {
                // Substitute silence of the recorded shape rather than dropping
                // the source, so every clip referencing it keeps its place.
                const SampleRate placeholderRate{sourceRate > 0.0 ? sourceRate : rate};
                audio = std::make_shared<const SilentSource>(
                    placeholderRate,
                    sourceChannels == 1   ? ChannelLayout::mono()
                    : sourceChannels == 2 ? ChannelLayout::stereo()
                                          : ChannelLayout::discrete(sourceChannels),
                    frames);
            }

            auto added = result.document.addSource(audio, name, resolved);
            if (!added) {
                result.warnings.emplace_back("could not register source " + name);
                continue;
            }
            sourceIds[rawId] = added.value();
            if (!failure.empty()) {
                result.missingSources.push_back(MissingSource{added.value(), resolved, failure});
            }
        }
    }

    std::uint64_t highestId = 0;
    if (root.contains("clips") && root["clips"].is_array()) {
        for (const Json& item : root["clips"]) {
            if (!item.is_object()) {
                result.warnings.emplace_back("dropped a clip entry that was not an object");
                continue;
            }

            const auto rawSource = item.value("source", std::uint64_t{0});
            const auto found = sourceIds.find(rawSource);
            if (found == sourceIds.end()) {
                result.warnings.emplace_back("dropped a clip naming source " +
                                             std::to_string(rawSource) +
                                             ", which the session does not contain");
                continue;
            }

            Clip clip;
            clip.source = found->second;
            if (!readCount(item, "length", clip.length) || clip.length <= 0) {
                result.warnings.emplace_back("dropped a clip with a non-positive length");
                continue;
            }
            if (!readCount(item, "sourceStart", clip.sourceStart) ||
                !readCount(item, "timelineStart", clip.timelineStart)) {
                result.warnings.emplace_back("dropped a clip with an invalid position");
                continue;
            }

            const auto rawId = item.value("id", std::uint64_t{0});
            clip.id = static_cast<ClipId>(rawId);
            highestId = std::max(highestId, rawId);

            const double gain = item.value("gain", 1.0);
            clip.gain = std::isfinite(gain) && gain >= 0.0 ? static_cast<float>(gain) : 1.0f;

            clip.fadeIn = fadeFromJson(item.value("fadeIn", Json::object()));
            clip.fadeOut = fadeFromJson(item.value("fadeOut", Json::object()));
            // A fade longer than its clip would read past the clip's end.
            clip.fadeIn.length = std::min(clip.fadeIn.length, clip.length);
            clip.fadeOut.length = std::min(clip.fadeOut.length, clip.length);
            clip.name = item.value("name", std::string{});

            result.document.timeline().insert(std::move(clip));
        }
    }

    if (root.contains("markers") && root["markers"].is_array()) {
        for (const Json& item : root["markers"]) {
            if (!item.is_object()) {
                continue;
            }
            Marker marker;
            if (!readCount(item, "position", marker.position)) {
                result.warnings.emplace_back("dropped a marker with an invalid position");
                continue;
            }
            readCount(item, "length", marker.length);
            marker.label = item.value("label", std::string{});
            result.document.markers().push_back(std::move(marker));
        }
    }

    // Never hand out an id a loaded clip already uses, whatever the file said.
    const auto recordedNextId = root.value("nextId", std::uint64_t{1});
    result.document.setNextId(std::max({recordedNextId, highestId + 1, result.document.nextId()}));
    return result;
}

Status saveSession(const Document& document, const std::filesystem::path& path) {
    auto json = sessionToJson(document, path.parent_path());
    if (!json) {
        return json.error();
    }

    std::ofstream stream{path, std::ios::trunc};
    if (!stream) {
        return Error{ErrorCode::IoFailure, "cannot write " + path.string()};
    }
    stream << json.value();
    stream.flush();
    if (!stream) {
        return Error{ErrorCode::IoFailure, "failed while writing " + path.string()};
    }
    return Status{};
}

Result<SessionLoadResult> loadSession(const std::filesystem::path& path, SourceResolver& resolver) {
    std::ifstream stream{path};
    if (!stream) {
        return Error{ErrorCode::IoFailure, "cannot open " + path.string()};
    }
    std::ostringstream contents;
    contents << stream.rdbuf();
    return sessionFromJson(contents.str(), resolver, path.parent_path());
}

Result<SessionLoadResult> loadSession(const std::filesystem::path& path) {
    FileSourceResolver resolver;
    return loadSession(path, resolver);
}

} // namespace sa::engine
