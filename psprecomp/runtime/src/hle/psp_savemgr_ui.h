// "Manage saves" screen of the savedata list dialog: import PSP save folders from PPSSPP / a memory stick / any
// folder into this game, and export this game's saves the other way (psp_savemgr.h does the copying). Menu screens
// are drawn with render_menu and the on-screen keyboard (psp_savedata_dialog.h), so it looks like the rest of the
// runtime's menus; the pad and the host keyboard both drive it. Pure logic + rendering, host-testable
// (tests/test_savemgr_ui.cpp).
#pragma once

#include "psp_savedata_dialog.h"
#include "psp_savemgr.h"

#include <functional>
#include <string>
#include <vector>

namespace psp_sddialog {

struct ManagerConfig {
    std::string saves_root;     // this game's SAVEDATA folder (ms0/PSP/SAVEDATA)
    std::string backup_root;    // where a replaced save's old copy goes (never inside saves_root: the game lists that)
    std::string disc_id;        // the running disc's ID: imported saves get it
    // The folder name already present for a save suffix under any shared region's ID ("" = none).
    std::function<std::string(const std::string& suffix)> existing;
    bool confirm_cross = true;
    // Where to look for other installs. Default (null) = psp_savemgr::detect_sources; tests inject their own.
    std::function<std::vector<psp_savemgr::Source>(bool with_saves)> detect;
};

class ManagerDialog {
public:
    void begin(const ManagerConfig& cfg);
    // Running while open; Cancelled when the user leaves the screen (Back on the first page).
    Result update(uint32_t buttons, int64_t now_ms);
    uint32_t revision() const;
    void render(Canvas& c) const;
    bool changed() const { return changed_; }   // saves were added or replaced: refresh the list behind it

    // ---- for tests ----
    enum class Screen { Main, PickSource, PickDest, TypePath, PickSaves, Confirm, Result };
    Screen screen() const { return screen_; }
    int selected() const { return sel_; }
    const std::vector<std::string>& result_lines() const { return result_; }

private:
    ManagerConfig cfg_;
    Screen screen_ = Screen::Main;
    bool importing_ = true;
    int sel_ = 0;
    std::vector<psp_savemgr::Source> sources_;
    std::vector<int> source_counts_;
    std::vector<psp_savemgr::SaveInfo> saves_;
    std::vector<bool> checked_;
    std::string path_;               // the chosen source (import) / destination (export)
    bool keep_region_ = true;        // export: keep each save's own disc ID
    std::string note_;               // one-line message shown on a page (errors)
    std::vector<std::string> result_, result_help_;
    std::vector<bool> result_ok_;
    std::vector<int> conflicts_;
    KeyboardDialog kbd_;
    bool changed_ = false, armed_ = false;
    uint32_t prev_ = 0, held_dir_ = 0;
    int64_t next_repeat_ = 0;
    uint32_t rev_ = 1;

    void go(Screen s, int sel = 0);
    void back();
    void activate();
    void adjust(int d);
    void move(int d);
    int row_count() const;
    void load_sources();
    bool open_path(const std::string& typed);
    void default_checks();
    void toggle(size_t i);
    void begin_run();
    void run(bool replace);
    std::vector<MenuRow> rows(std::string* title, std::string* tab) const;
    int start_row() const;
};

}  // namespace psp_sddialog
