// Asset resolution: turns a forgiving name like "click.wav" into a file on
// disk by searching an ordered list of roots. Not part of the public API; the
// public surface is AddAssetRoot and friends in ludifex.h.

#pragma once

#include <ludifex/ludifex.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ludifex::detail
{

// Returns the first existing candidate, or an empty string. On failure every
// candidate that was tried is logged under the given category.
std::string ResolveAsset(const std::string& path, const char* category,
                         LogLevel failureLevel = LogLevel::Error);

// Resolves and reads a whole file. Returns false, having reported why, when
// the file cannot be found or read.
bool LoadAssetFile(const std::string& path, const char* category, std::vector<uint8_t>& outBytes,
                   std::string* outResolvedPath = nullptr, LogLevel failureLevel = LogLevel::Error);

// ---------------------------------------------------------------------------
// Watching a file, and knowing what is in it
//
// Hot reload asks "has this changed?" every few frames, and the caches ask "is
// this the same asset?" when two names might refer to one file. A timestamp
// answers the first cheaply but not reliably (saving a file without editing
// it changes the timestamp); a hash of the contents answers both but costs a
// read.
//
// So both are used: the timestamp decides whether to read the file, and the
// hash decides whether its contents changed. An untouched file costs one
// stat.
// ---------------------------------------------------------------------------

// FNV-1a over the file's bytes. Zero when the file cannot be read, which is
// never a valid hash, so zero means "unknown" everywhere it appears.
//
// Not a cryptographic hash. A collision would cause a wrong cache hit, nothing
// worse.
uint64_t HashFileContents(const std::string& resolvedPath);

// The same hash over bytes already in memory, for an asset that was never a
// file, such as an image embedded in a .glb.
uint64_t HashBytes(const void* bytes, size_t size);

int64_t FileTimestamp(const std::string& resolvedPath);

// What a cached asset remembers about the file it came from.
struct FileWatch
{
    std::string Path;          // resolved, so the watch survives a root change
    int64_t Timestamp = 0;
    uint64_t Hash = 0;
};

// Fills in the timestamp and hash for a file being cached for the first time.
void BeginWatching(FileWatch& watch, const std::string& resolvedPath);

// True when the file's contents differ from the ones the watch was taken on,
// in which case the watch is updated to the new ones. A moved timestamp with
// unchanged contents updates the timestamp and returns false, so re-saving an
// unedited file costs a read rather than a reload.
bool HasFileChanged(FileWatch& watch);

// Whether a changed file should be picked up while the program runs. Off by
// default; the public switch is SetAssetHotReload. It lives beside the
// watching because it covers every kind of asset, and the caches check it
// too.
void SetHotReloadEnabled(bool enabled);
bool IsHotReloadEnabled();

// True at most a few times a second, and never while hot reload is off, so
// the file system is never checked every frame.
bool ShouldCheckForChanges();

} // namespace ludifex::detail
