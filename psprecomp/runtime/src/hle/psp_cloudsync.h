// Cloud sync of save folders between installs (Windows, Linux, Android ...) through a pluggable storage provider.
// Pure library (std::filesystem, no SDL, no network): the providers that talk to a service (WebDAV, Google Drive ...)
// implement `Provider`; the local-folder provider here covers any sync client (Syncthing, a Drive/OneDrive folder, a
// NAS mount) and the tests.
//
// What is stored: per save, keyed by its REGION-NEUTRAL SUFFIX ("GameData00", "FCard03", so a US and an EU install
// sync with each other):
//   <suffix>.save   the files that travel with the save folder, in one container (pack/unpack below)
//   <suffix>.meta   text: content hash, container checksum, PARAM.SFO mtime, the device that wrote it, the disc ID it was saved under
// The blob is written first and the meta last, so a half-finished upload is never seen as a newer save.
//
// Deciding what to do (three-way): each install remembers the hash of every save as of its last sync (the "base").
//   local == remote            nothing to do
//   only one side has it       copy it over (a save is never deleted by a sync)
//   remote == base             only local changed  -> push
//   local  == base             only remote changed -> pull
//   otherwise                  both changed (or no base to tell): a CONFLICT, which is never resolved silently
// The content hash covers the data files but not PARAM.SFO (its folder name differs between regions); the data
// files change whenever the game saves. Pulling retargets and re-signs PARAM.SFO (psp_savemgr::copy_save), and the
// save it replaces is moved to the backup folder; the remote copy a push replaces in a conflict is kept as
// <suffix>.bak.
#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace psp_cloud {

using Bytes = std::vector<uint8_t>;

// ---- storage provider: a flat folder of small named objects. Every call returns "" or an error text. ----
struct RemoteItem {
    std::string name;
    uint64_t size = 0;
};

class Provider {
public:
    virtual ~Provider() = default;
    virtual std::string label() const = 0;                          // "Folder /home/me/Sync", "Google Drive"
    virtual std::string list(std::vector<RemoteItem>* out) = 0;
    virtual std::string get(const std::string& name, Bytes* out) = 0;
    virtual std::string put(const std::string& name, const Bytes& data) = 0;   // create or replace
    virtual std::string remove(const std::string& name) = 0;        // a missing object is not an error
};

// A folder on this machine (or any mounted/synced one). Created on first use.
std::unique_ptr<Provider> make_folder_provider(const std::string& dir);

// ---- hashing and the save container ----
std::string sha256_hex(const Bytes& data);

// Hash of a save folder's data files (everything psp_savemgr::save_files lists except PARAM.SFO): name, length and
// content of each in name order. "" when the folder has no data files.
std::string hash_save(const std::string& dir);

// Packs the files that travel with `dir` (PARAM.SFO included). "" on success.
std::string pack_save(const std::string& dir, Bytes* out);
// Unpacks into the (new, empty) folder `dir`. Refuses names that are not plain file names. "" on success.
std::string unpack_save(const Bytes& blob, const std::string& dir);

// ---- the remote metadata of one save ----
struct Meta {
    std::string hash;      // content hash of the data files (hash_save)
    std::string blob;      // sha256 of the whole .save container: catches damage in PARAM.SFO and the icons too ("" = old meta)
    std::string device;
    std::string disc;
    time_t mtime = 0;
    bool valid() const { return !hash.empty(); }
};
std::string meta_to_text(const Meta& m);
Meta meta_from_text(const std::string& text);

// ---- synchronising ----
enum class Action {
    Same,        // identical on both sides
    Pushed,      // uploaded
    Pulled,      // downloaded (the old local save went to the backup folder)
    Conflict,    // both changed: nothing was done, see Item::local / Item::remote
    Skipped,     // the user chose to leave it alone for now
    Error,       // Item::note says why
};

struct Item {
    std::string suffix;
    Action action = Action::Same;
    std::string note;        // error text, or a short description for the result list
    time_t local_mtime = 0;  // for the conflict screen
    Meta remote;
};

enum class Choice { KeepLocal, KeepRemote, Skip };

struct Options {
    std::string saves_root;      // this install's SAVEDATA folder
    std::string backup_root;     // where a replaced local save goes (not inside saves_root: the game lists that)
    std::string state_path;      // the file that remembers the base hashes and this device's ID (created)
    std::string disc_id;         // disc ID under which a pulled save is created locally
    // The local folder name already holding `suffix` under any shared region's ID ("" = none). Null: disc_id+suffix.
    std::function<std::string(const std::string& suffix)> existing;
    // Conflicts: null = report them (Action::Conflict) and do nothing; else decide here.
    std::function<Choice(const Item&)> on_conflict;
    // Only these suffix kinds: false keeps out replays (big, rarely worth carrying). Default: everything.
    std::function<bool(const std::string& suffix)> include;
    // false = push-only run (after the game saved): a save that would be downloaded is left alone (Action::Skipped), so a
    // sync during play never replaces a save the game may be using.
    bool allow_pull = true;
};

// Compares every local save with the remote ones and acts as described above. Returns one Item per save seen on
// either side. `*error` (when non-null) gets "" or the text of a failure that stopped everything (listing failed).
std::vector<Item> sync_all(Provider& p, const Options& o, std::string* error = nullptr);

// Applies a user's choice to one save of an earlier sync_all that reported a conflict. Returns the finished Item.
Item resolve_conflict(Provider& p, const Options& o, const std::string& suffix, Choice c);

// This device's ID (generated into the state file on first use).
std::string device_id(const std::string& state_path);

}  // namespace psp_cloud
