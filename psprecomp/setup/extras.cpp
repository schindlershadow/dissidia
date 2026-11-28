#include "extras.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <vector>

#include "install.h"

namespace setup {
namespace fs = std::filesystem;

namespace {
std::string join(const std::string& a, const std::string& b) { return (fs::path(a) / b).string(); }

std::string esc(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c == '\n') o += "\\n";
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o + "\"";
}

// ---- the list. Order = order on the pages and of the install steps.
std::vector<Extra> make_extras() {
    std::vector<Extra> v;

    {   // HD textures: our pack (D012FFRT + our upscales), downloaded into mods/texpacks/recomp-hd. Several packs can be installed side by
        // side (mods/texpacks/<id>, runtime/include/psp_texpacks.h); the game applies one, picked in Settings > Graphics > Texture Pack.
        Extra e;
        e.key = "hd_textures";
        e.need_bytes = 3LL << 30;
        e.feature = "hd_textures";
        e.title = "HD textures (Dissidia Recomp HD)";
        e.help = "Downloads our HD texture pack (about 1.3 GB): the D012FFRT community pack plus our own AI upscales of the other textures. "
                 "Credits: D012FFRT by Niosa (github.com/Niosa/D012FFRT); AI upscales by schindlershadow made with Real-ESRGAN (Xintao Wang et al.). "
                 "More packs can be added below; the game uses one, picked in Settings > Graphics > Texture Pack.";
        e.android_help = " The download goes over your connection (WiFi is best) and needs about 3 GB of free space while it unpacks.";
        e.icon = "texture";
        e.def = "1";
        e.reapply = Extra::Redo;
        // Our pack: mods/texpacks/recomp-hd in the game's folder (Android: the game's own copy; the work folder keeps none), or the one pack
        // an older release kept in mods/textures (fetch_textures.py moves it into texpacks the next time it runs): no new download for it.
        e.present = [](const ExtraCtx& c) {
            std::error_code ec;
            for (const fs::path& base : {fs::path(c.game_files()), fs::path(c.workdir)})
                if (fs::is_directory(base / "mods/texpacks/recomp-hd", ec) || fs::is_regular_file(base / "mods/textures/textures.ini", ec)) return true;
            return false;
        };
        e.remove = [](const ExtraCtx& c) {   // only our pack: packs the player added stay
            std::error_code ec;
            for (const fs::path& base : {fs::path(c.game_files()), fs::path(c.workdir)}) {
                fs::remove_all(base / "mods/texpacks/recomp-hd", ec);
                std::ifstream src(base / "mods/textures/.source.json");   // an older release's single pack, if it is ours
                std::string j((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
                if (j.find("\"pack\": \"ai\"") != std::string::npos) fs::remove_all(base / "mods/textures", ec);
            }
        };
        e.steps = [](const ExtraCtx& c, const Options&, std::vector<Step>& p) {
            if (!c.android) {   // fetch_textures.py downloads and unpacks in one go
                p.push_back(c.tool("Downloading the HD textures", "Downloads the Dissidia Recomp HD textures (about 1.3 GB).",
                                   "fetch_textures.py", {c.workdir, "--pack", "ai"}, 0.06));
                return;
            }
            const std::string mods = join(c.workdir, "mods");
            std::error_code ec;
            fs::create_directories(mods, ec);
            const std::vector<std::pair<std::string, std::string>> files = {
                {"https://codeload.github.com/Niosa/D012FFRT/zip/main", join(mods, ".d012ffrt.zip")},
                {"https://codeload.github.com/schindlershadow/dissidia-hd-textures/zip/main", join(mods, ".hdtextures.zip")}};
            p.push_back(c.download("Downloading the HD textures", "Downloads the Dissidia Recomp HD textures (about 1.3 GB). WiFi is best.", files, 0.25));
            p.push_back(c.tool("Unpacking the HD textures", "Unpacks the textures into the mods folder.", "fetch_textures.py",
                               {c.workdir, "--pack", "ai", "--consume-zips", "--zip", join(mods, ".d012ffrt.zip"), "--ids-zip", join(mods, ".hdtextures.zip")}, 0.06));
        };
        v.push_back(e);
    }
    {   // the player's own texture pack archive, added next to the installed packs (an action: it can be done again for another pack)
        Extra e;
        e.key = "texture_add";
        e.need_bytes = 1LL << 30;
        e.feature = "hd_textures";
        e.title = "Add your own texture pack";
        e.help = "Installs a texture pack you downloaded yourself, as one compressed file: a PPSSPP-style pack for Dissidia 012 (a textures.ini with "
                 "its PNG files, in any folder of the archive). You pick the file next. It is added next to the packs already installed and used "
                 "from the next start; Settings > Graphics > Texture Pack in the game switches between them. Turn this on again to add another.";
        e.icon = "texture";
        e.def = "0";
        e.action = true;
        e.disc = {"texture_file", "Your texture pack", "Texture pack", "A texture pack file",
                  "A compressed Dissidia 012 texture pack in the PPSSPP format (a textures.ini and its PNG files). Packs made for the US release (ULUS10566) fit best; "
                  "most textures are shared with the European release. It is only read once.",
                  "Valid: a pack with a textures.ini",
                  "The archive must hold a textures.ini (in any folder), or PNG files named by the game's texture ids. On a PC .7z and .rar work too when 7-Zip "
                  "(or Windows' own tar) can unpack them; on Android pick a .zip.",
                  ".zip (PC: also .7z / .rar)", "textures.ini"};
        e.disc.patterns = {".zip", ".7z", ".rar"};
        e.disc.filter_name = "Texture packs";
        e.disc.check = texture_pack_problem;
        e.steps = [](const ExtraCtx& c, const Options& o, std::vector<Step>& p) {
            std::vector<std::string> args = {c.workdir, "--pack", "file", "--zip", o.get("texture_file")};
            if (!o.get("texture_file_name").empty()) args.insert(args.end(), {"--name", o.get("texture_file_name")});   // Android: the picked file's name
            p.push_back(c.tool("Installing your texture pack", "Unpacks the texture pack you picked next to the installed packs.", "fetch_textures.py", args, 0.06));
        };
        v.push_back(e);
    }
    {   // Japanese voices from an undub / Japanese disc
        Extra e;
        e.key = "jp_audio";
        e.need_bytes = 1LL << 30;
        e.feature = "jp_audio";
        e.title = "Japanese voices (undub)";
        e.help = "Adds the Japanese voices and movies from a second disc image (an undub ISO) that you pick next.";
        e.icon = "voice";
        e.disc = {"undub_iso", "The undub file", "Japanese voices", "A second disc image",
                  "The Japanese voices and movies come from a Japanese or undub version of Dissidia 012, as a disc image: a .iso or .cso. It is only read once; your own disc stays the base of the game.",
                  "Valid: the undub / Japanese disc",
                  "A Dissidia 012 image that has the Japanese voices, for example the Japanese release or an undub patch of the game. The same game as your own disc, just with different voice and movie files. A disc with the English voices, or another game, is refused with a message.",
                  ".iso or .cso", "Japanese / undub"};
        e.disc.check = undub_disc_problem;   // a Dissidia 012 without the English voices
        e.steps = [](const ExtraCtx& c, const Options& o, std::vector<Step>& p) {
            p.push_back(c.tool("Extracting the Japanese voices", "Adds the Japanese voices and movies from the undub disc.", "jp_audio.py",
                               {"--iso", o.get("undub_iso"), "--work", c.workdir}, 0.05));
        };
        v.push_back(e);
    }
    {   // Bartz's eight moves of Dissidia (2008), rebuilt from the player's two discs
        Extra e;
        e.key = "d1_moves";
        e.need_bytes = 1LL << 30;
        e.feature = "d1_moves";
        e.title = "Bartz: Dissidia 1 moves";
        e.help = "Gives Bartz back the eight moves he had in the first Dissidia (Soul Eater, Flood, Wind Shear, Paladin Force...), next to his 012 kit. "
                 "Needs your Dissidia 1 (2008) disc image, which you pick next. In the game, Settings > Mods turns them off and on. Online, players without them "
                 "receive what they need from you.";
        e.icon = "sword";
        e.disc = {"d1_iso", "Dissidia 1 disc", "Bartz's Dissidia 1 moves", "A Dissidia 1 disc image",
                  "Bartz's eight moves from the first Dissidia (Soul Eater, Flood, Paladin Force...) are rebuilt from your own copy of Dissidia Final Fantasy (2008), as a .iso disc image. It is only read once. The app ships only small patches that need both discs to turn into the moves.",
                  "Valid: the US release",
                  "Dissidia Final Fantasy (USA, ULUS-10437) as an uncompressed .iso, together with a US Dissidia 012. Unpack .zip / .7z first. Other releases are refused with a message; the rest of the install is not affected.",
                  ".iso", "US"};
        e.disc.check = d1_disc_problem;   // Dissidia Final Fantasy (USA, ULUS-10437) only
        e.steps = [](const ExtraCtx& c, const Options& o, std::vector<Step>& p) {
            p.push_back(c.tool("Adding Bartz's Dissidia 1 moves",
                               "Rebuilds the eight moves Bartz had in the first Dissidia from your Dissidia 1 disc and adds them as new abilities. Settings > Mods in the game turns them off and on.",
                               "d1_moves.py", {"install", "--d1", o.get("d1_iso"), "--work", c.workdir}, 0.01));
        };
        v.push_back(e);
    }
    return v;
}

bool offered(const Extra& e, const Layout& l, bool android) {
    return (android ? e.android : e.desktop) && (e.feature.empty() || feature_available(l, e.feature));
}

bool is_on(const Extra& e, const Options& o) {
    std::string v = o.get(e.key);
    return e.choices.empty() ? v == "1" : !v.empty() && v != "0";
}

// Does `e` need its disc / file for these options? On (and, for a sub-choice, its parent on) and, with ExtraDisc::when, set to that value.
bool disc_wanted(const Extra& e, const Options& o) {
    if (e.disc.key.empty() || !is_on(e, o)) return false;
    if (!e.parent.empty()) {
        const Extra* parent = find_extra(e.parent);
        if (parent && !is_on(*parent, o)) return false;
    }
    return e.disc.when.empty() || o.get(e.key) == e.disc.when;
}

// a sub-choice (texture pack) changed
bool children_changed(const Extra& e, const Options& now, const Options& prev) {
    for (const Extra& c : extras())
        if (c.parent == e.key && now.get(c.key) != prev.get(c.key)) return true;
    return false;
}
}  // namespace

const std::vector<Extra>& extras() {
    static const std::vector<Extra> list = make_extras();
    return list;
}

const Extra* find_extra(const std::string& key) {
    for (const Extra& e : extras())
        if (e.key == key) return &e;
    return nullptr;
}

std::string Options::get(const std::string& key) const {
    const Extra* e = find_extra(key);
    auto it = extras.find(key);
    if (it != extras.end()) {
        if (!e || e->choices.empty()) return it->second;
        for (const ExtraChoice& c : e->choices)   // a choice a newer release no longer has (a pack an older release offered): the default
            if (c.value == it->second) return it->second;
    }
    return e ? e->def : "";
}

void Options::clear_discs() {   // after a run: picked files are no longer valid, and actions (add a texture pack) are done
    for (const Extra& e : setup::extras()) {
        if (!e.disc.key.empty()) extras.erase(e.disc.key);
        if (e.action) extras.erase(e.key);
    }
    extras.erase("texture_file_name");
}

void Options::clear_actions() {
    for (const Extra& e : setup::extras())
        if (e.action) {
            extras.erase(e.key);
            if (!e.disc.key.empty()) extras.erase(e.disc.key);
        }
    extras.erase("texture_file_name");
}

Step ExtraCtx::tool(const std::string& label, const std::string& explain, const std::string& script, const std::vector<std::string>& args, double weight) const {
    Step s;
    s.label = label;
    s.explain = explain;
    s.argv = {l.python, join(l.game_dir, "tools/" + script)};
    s.argv.insert(s.argv.end(), args.begin(), args.end());
#ifdef __ANDROID__
    if (android) s.env = android_env(l, workdir);
    else
#endif
        s.env = {{"PYTHONUNBUFFERED", "1"}};
    s.weight = weight;
    return s;
}

Step ExtraCtx::download(const std::string& label, const std::string& explain, const std::vector<std::pair<std::string, std::string>>& files, double weight) const {
    if (android) {
        Step s;
        s.label = label;
        s.explain = explain;
        s.downloads = files;
        s.weight = weight;
        return s;
    }
    std::vector<std::string> args;
    for (auto& f : files) { args.push_back(f.first); args.push_back(f.second); }
    return tool(label, explain, "download.py", args, weight);
}

std::string ExtraCtx::game_files() const {
#ifdef __ANDROID__
    if (android) return android_game_dir();
#endif
    return workdir;
}

void plan_extra_steps(std::vector<Step>& p, const ExtraCtx& ctx, const Options& now, const Options* prev) {
    for (const Extra& e : extras()) {
        if (!e.parent.empty() || !e.steps || !offered(e, ctx.l, ctx.android)) continue;   // sub-choices belong to their parent's steps
        const bool on = is_on(e, now), was = prev && is_on(e, *prev);
        bool missing = false;   // a sub-choice built from a file (the own texture pack) that was not picked: nothing to install it from,
        for (const Extra& c : extras())   // so the installed variant stays as it is
            if (c.parent == e.key && disc_wanted(c, now) && now.get(c.disc.key).empty()) missing = true;
        if (on && missing) continue;
        if (e.reapply == Extra::Redo && prev) {
            const bool changed = !was || children_changed(e, now, *prev);
            const bool there = e.present ? e.present(ctx) : false;
            if (e.remove && (was || there) && (!on || changed)) e.remove(ctx);   // off, or another variant: the old files must not stay
            if (!on || (!changed && there)) continue;
        } else if (!on) {
            continue;
        } else if (prev && was && e.disc.key.empty()) {
            continue;   // Once, no disc: already added
        }
        if (!e.disc.key.empty() && now.get(e.disc.key).empty()) continue;   // already added earlier (no new disc), or not picked
        e.steps(ctx, now, p);
    }
}

std::vector<std::string> extras_needing_disc(const Layout& l, bool android, const Options& now, const Options* prev) {
    std::vector<std::string> keys;
    for (const Extra& e : extras()) {
        if (!offered(e, l, android) || !disc_wanted(e, now)) continue;
        if (prev && disc_wanted(e, *prev) && !e.action) continue;   // already added from that file: not needed again (its files stay)
        if (now.get(e.disc.key).empty()) keys.push_back(e.key);
    }
    return keys;
}

std::string extra_disc_problem(const std::string& key, const std::string& path) {
    const Extra* e = find_extra(key);
    return e && e->disc.check ? e->disc.check(path) : "";
}

std::string find_extra_disc(const std::string& key, const std::string& near_file) {
    const Extra* e = find_extra(key);
    if (!e || !e->disc.check || e->disc.patterns.empty() || e->disc.patterns.front() != ".iso" || near_file.empty() || near_file.compare(0, 3, "fd:") == 0) return "";
    std::error_code ec;
    const fs::path dir = fs::path(near_file).parent_path();
    if (dir.empty() || !fs::is_directory(dir, ec)) return "";
    std::vector<std::string> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || it->file_size(e2) < 32768u) continue;   // not an empty / tiny file (a disc image is hundreds of MB)
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        for (const std::string& p : e->disc.patterns) if (ext == p) { files.push_back(it->path().string()); break; }
    }
    std::sort(files.begin(), files.end());
    if (files.size() > 40) files.resize(40);
    for (const std::string& f : files)
        if (e->disc.check(f).empty()) return f;   // the id / voices match: this is the disc the extra needs
    return "";
}

std::string extras_json(const Layout& l, bool android) {
    std::string o = "[";
    bool first = true;
    for (const Extra& e : extras()) {
        if (!(android ? e.android : e.desktop)) continue;
        if (!first) o += ",";
        first = false;
        o += "{\"key\":" + esc(e.key) + ",\"title\":" + esc(e.title) + ",\"help\":" + esc(e.help + (android ? e.android_help : "")) + ",\"icon\":" + esc(e.icon) +
             ",\"def\":" + esc(e.def) + ",\"parent\":" + esc(e.parent) + ",\"action\":" + (e.action ? "true" : "false") + ",\"available\":" + (offered(e, l, android) ? "true" : "false") + ",\"choices\":[";
        for (size_t i = 0; i < e.choices.size(); i++)
            o += (i ? "," : "") + std::string("{\"value\":") + esc(e.choices[i].value) + ",\"label\":" + esc(e.choices[i].label) + ",\"help\":" + esc(e.choices[i].help) + "}";
        o += "]";
        if (!e.disc.key.empty())
            o += ",\"disc\":{\"key\":" + esc(e.disc.key) + ",\"title\":" + esc(e.disc.page_title) + ",\"eyebrow\":" + esc(e.disc.eyebrow) + ",\"what\":" + esc(e.disc.what) +
                 ",\"what_help\":" + esc(e.disc.what_help) + ",\"valid\":" + esc(e.disc.valid) + ",\"valid_help\":" + esc(e.disc.valid_help) + ",\"exts\":" + esc(e.disc.exts) + ",\"valid_short\":" + esc(e.disc.valid_short) + ",\"when\":" + esc(e.disc.when) + ",\"filter_name\":" + esc(e.disc.filter_name) + ",\"patterns\":[" + [&] {
                     std::string a;
                     for (size_t i = 0; i < e.disc.patterns.size(); i++) a += (i ? "," : "") + esc(e.disc.patterns[i]);
                     return a; }() + "]}";
        o += "}";
    }
    return o + "]";
}

}  // namespace setup
