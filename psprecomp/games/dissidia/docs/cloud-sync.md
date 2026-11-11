# Cloud sync of saves

Keeps the save folders of several installs (Windows, Linux, Android; any region) in step through a synced **folder**, a
**WebDAV** server (Nextcloud, ownCloud, most NAS boxes) or **Google Drive**. It is built in: no extra program.

## Using it

- **Settings menu** (F1 / Guide button) > **Cloud** page: pick the provider and fill in what it needs.
  - *Folder*: any folder another program keeps in sync (Syncthing, the Drive / OneDrive / Dropbox desktop apps, a NAS share).
  - *WebDAV*: the address of a folder on the server (it must be inside an existing folder), user and password. For
    Nextcloud / ownCloud create an **app password** (Settings > Security) and use that.
  - *Google Drive*: **Sign in...** shows a code; enter it at google.com/device on a phone or computer. The game only gets
    Google's `drive.appdata` permission: its own hidden folder in your Drive, never your other files.
- **Save list** (the game's load / save screen): **Triangle > Cloud sync** shows the status, **Sync now**, the Google
  sign-in, and the choices when a save changed on two devices. This is the safest place to download: the game is not reading
  saves while the list is open. Saves downloaded here show up in the list at once.
- `PSPRECOMP_CLOUD=0` switches the whole feature off (nothing is read, started or contacted).

## When it syncs

| When | What |
|---|---|
| Start of the game | a full sync (uploads and downloads), in the background. The game waits for it (up to 20 s) before its first save dialog, so a download never replaces a save the game is reading |
| A few seconds after the game saves | **push only**: your save goes up; newer saves from other devices are *not* downloaded during play (they come at the next start or **Sync now**) |
| **Sync now** | a full sync |

*Sync Automatically: Off* leaves only **Sync now**. Replays are left out unless *Include Replays* is on.

## Conflicts

Each install remembers the content hash of every save as of its last sync. A save is uploaded when only this device changed it,
downloaded when only the cloud copy changed, and a **conflict** when both did (or there is no history to tell, e.g. a new
install that already has its own save). A conflict is **never resolved silently**: it waits in *Triangle > Cloud sync* until you
choose *keep this device's save* (the cloud copy is kept once as `<slot>.bak` in the cloud), *take the cloud save* (yours goes to
`ms0/PSP/SAVEDATA_BACKUP/`) or *decide later*. A sync never deletes a save, locally or in the cloud.

Saves are matched by their slot name without the region ID (`GameData00`, `FCard03`...), so a US and an EU install sync with each
other; a downloaded save is renamed to the installed game's ID and its `PARAM.SFO` signed again (`psp_savemgr`).

## Where things are stored

| What | Where |
|---|---|
| Settings, secrets (WebDAV password, Google sign-in token) | `<workdir>/cloud.ini` (mode 0600 on Linux/Android; keep it private). Not in `settings.ini`, which is logged and copied around |
| Last-synced hashes, this device's ID | `<workdir>/cloud/state.txt` |
| Replaced local saves | `ms0/PSP/SAVEDATA_BACKUP/` |
| In the cloud | per slot `<slot>.save` (the files in one container), `<slot>.meta` (content hash, container checksum, mtime, device, disc ID), optional `<slot>.bak` |

The `.save` is written first and the `.meta` last, so a half-finished upload is never taken for a newer save; a download is
checked against both hashes before it touches anything.

## Google Drive client (for forks)

`runtime/src/hle/psp_cloud_keys.h` holds this project's OAuth client (type *TVs and Limited Input devices*, scope
`drive.appdata`). Google does not treat that client secret as confidential. A fork should create its own in the Google Cloud
console (APIs & Services: enable the Drive API, OAuth consent screen with the `drive.appdata` scope and a privacy policy page,
create the client) and replace the two values, or set `PSPRECOMP_GOOGLE_CLIENT_ID` / `_SECRET`.

## Code

| File | Role |
|---|---|
| `psp_cloudsync.{h,cpp}` | provider interface, folder provider, SHA-256, save container, the three-way sync and conflict handling |
| `psp_cloud_providers.{h,cpp}` | WebDAV and Google Drive (device sign-in, refresh, `appDataFolder`) |
| `psp_http.{h,cpp}` | blocking HTTP/1.1 client; HTTPS through mbedTLS with the embedded Mozilla CA bundle (`assets/ca`), certificate and host name checked |
| `psp_cloud_service.{h,cpp}` | configuration, worker thread, status, debounced push after a save |
| `psp_cloud_glue.{h,cpp}` | the few calls the runtime makes (boot, wait before the first save dialog, after a save, shutdown) |
| `psp_savemgr_ui.cpp`, `psp_settings.cpp` | the two screens |

HTTPS needs **mbedTLS 3.6+** (optional at build time: without it Folder sync still works and https reports "no TLS support").
The toolchain bundle builds it from a pinned source (`toolchain/versions.json`); for a developer build pass
`-DPSPRECOMP_MBEDTLS_PREFIX=<install prefix>`.

## Tests (no network, no accounts)

`test_cloudsync` (two installs through a folder: upload, cross-region download, conflicts, backups, damaged downloads, unsafe
containers), `tests/run_http.sh` (HTTP/TLS client against a local server: chunked and close-delimited bodies, redirects,
timeouts, truncation, an untrusted certificate and a wrong host name refused), `tests/run_cloud.sh` (WebDAV and Google Drive
against a fake server, the service with its real worker thread, sign-in, refresh, revoked sign-in), `test_savemgr` (the Cloud
screens with a fake service).
