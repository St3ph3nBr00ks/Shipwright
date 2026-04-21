// Voice-pack backend prototype (B2 day-1 probe) — see VoicePack_prototype.h.

#include "VoicePack_prototype.h"

#include <libultraship/libultraship.h>
#include <ship/Context.h>
#include <ship/resource/archive/Archive.h>
#include <ship/resource/archive/OtrArchive.h>
#include <ship/resource/archive/O2rArchive.h>
#include <spdlog/spdlog.h>

#include "soh/OTRGlobals.h"  // appShortName

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

// Paths this prototype considers "audio-shaped".  Matches what AudioCollection
// and audio_load.c already scan at startup, plus the plan's proposed coop
// conventions.  Loose matching (substring) because a pack author may namespace
// differently than expected — we want to see EVERYTHING audio-flavored so the
// B2 decision has the full picture.
const char* kAudioPathHints[] = {
    "custom/fonts/",
    "custom/music/",
    "audio/fonts",
    "audio/sequences",
    "audio/samples",
    "audio/",
    "coopaudio/",
};

bool PathLooksAudio(const std::string& p) {
    for (const char* h : kAudioPathHints) {
        if (p.find(h) != std::string::npos) return true;
    }
    // Common voice-SFX hint: anything with "VO" or "voice" near a Link-id substring
    if (p.find("NA_SE_VO_LI") != std::string::npos) return true;
    if (p.find("voice") != std::string::npos) return true;
    if (p.find("Voice") != std::string::npos) return true;
    return false;
}

std::shared_ptr<Ship::Archive> OpenTransient(const std::string& archivePath, const std::string& ext) {
    std::shared_ptr<Ship::Archive> archive;
    if (ext == ".o2r" || ext == ".zip") {
        archive = std::make_shared<Ship::O2rArchive>(archivePath);
    } else {
        archive = std::make_shared<Ship::OtrArchive>(archivePath);
    }
    archive->Load();
    return archive->IsLoaded() ? archive : nullptr;
}

// Walks a pack folder, opens each archive transiently, and logs every file
// whose path looks audio-related.  Transient opens are released when the
// function returns — no ArchiveManager state touched.
void ScanPack(const std::string& folder) {
    const std::string coopRoot =
        Ship::Context::LocateFileAcrossAppDirs("coopplayercharacters", appShortName);
    const std::filesystem::path charDir = std::filesystem::path(coopRoot) / folder;

    if (!std::filesystem::exists(charDir) || !std::filesystem::is_directory(charDir)) {
        SPDLOG_WARN("[VoicePack][Probe] folder \"{}\" does not exist under \"{}\"",
                    folder, coopRoot);
        return;
    }

    SPDLOG_INFO("[VoicePack][Probe] --- begin scan: folder=\"{}\" charDir=\"{}\" ---",
                folder, charDir.generic_string());

    int archivesScanned = 0;
    int audioCandidateFiles = 0;
    std::vector<std::string> perArchiveHints;  // for a compact summary at the end

    for (const auto& entry : std::filesystem::recursive_directory_iterator(charDir)) {
        if (entry.is_directory()) continue;
        const std::string ext = entry.path().extension().generic_string();
        if (ext != ".otr" && ext != ".o2r") continue;

        const std::string archivePath = entry.path().generic_string();
        auto archive = OpenTransient(archivePath, ext);
        if (!archive) {
            SPDLOG_WARN("[VoicePack][Probe]   failed to open \"{}\"", archivePath);
            continue;
        }
        archivesScanned++;

        auto files = archive->ListFiles();
        if (!files) {
            SPDLOG_WARN("[VoicePack][Probe]   no file list for \"{}\"", archivePath);
            continue;
        }

        int archiveTotal = 0;
        int archiveAudio = 0;
        for (auto& [hash, path] : *files) {
            archiveTotal++;
            if (PathLooksAudio(path)) {
                archiveAudio++;
                audioCandidateFiles++;
                // Log every audio candidate path so we can see exactly what
                // convention the pack author used.  Intentionally verbose for
                // the probe — B2 proper will summarize.
                SPDLOG_INFO("[VoicePack][Probe]   [{}] {}",
                            std::filesystem::path(archivePath).filename().generic_string(),
                            path);
            }
        }
        perArchiveHints.push_back(std::filesystem::path(archivePath).filename().generic_string() +
                                  " (" + std::to_string(archiveAudio) + "/" +
                                  std::to_string(archiveTotal) + " audio-like)");
    }

    SPDLOG_INFO("[VoicePack][Probe] --- end scan: folder=\"{}\" archives={} audioCandidates={} ---",
                folder, archivesScanned, audioCandidateFiles);
    for (const auto& h : perArchiveHints) {
        SPDLOG_INFO("[VoicePack][Probe]   per-archive: {}", h);
    }

    if (audioCandidateFiles == 0) {
        SPDLOG_WARN("[VoicePack][Probe] folder \"{}\" has {} archives but zero audio-shaped files. "
                    "Either the pack has no voice content, or the path convention is one we didn't "
                    "probe for.  Hint set: custom/fonts/, custom/music/, audio/fonts, audio/sequences, "
                    "audio/samples, audio/, coopaudio/, NA_SE_VO_LI*, *voice*.",
                    folder, archivesScanned);
    }
}

} // namespace

extern "C" void VoicePack_Prototype_OnAudioModChanged(const char* folder) {
    if (folder == nullptr) {
        SPDLOG_INFO("[VoicePack][Probe] OnAudioModChanged: null folder (no-op)");
        return;
    }
    const std::string f = folder;
    if (f.empty()) {
        SPDLOG_INFO("[VoicePack][Probe] OnAudioModChanged: Default Voices selected (no-op)");
        return;
    }
    SPDLOG_INFO("[VoicePack][Probe] OnAudioModChanged: folder=\"{}\"", f);
    ScanPack(f);
}
