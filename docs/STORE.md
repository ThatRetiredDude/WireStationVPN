# Publishing on the Prospero Store

WireStationVPN is listed in the
[PS5 homebrew catalog](https://github.com/darkxex/ps5-homebrew-catalog) as
**PPSA99556**, the same way TokenStation5 is.

## A release

1. Bump `contentVersion` in `app/sce_sys/param.json` (PlayStation format,
   such as `01.000.001`) and merge it.
2. Tag that commit with the same string and push the tag:
   `git tag 01.000.001 && git push origin 01.000.001`.
3. The **release** workflow tests and builds everything, then publishes the
   release with `PPSA99556.zip` (the `PPSA99556/` app folder at the root),
   its SHA-256, and `PPSA99556.json`, the catalog record with the real link
   and hash.
4. From the ThatRetiredDude account, open a catalog PR that changes only
   `apps/PPSA99556.json` (the first time: adds it) with that record.

Never replace a listed release's ZIP: its SHA-256 would stop matching.

## The record

| Field | Value |
| --- | --- |
| `titleid` | `PPSA99556` |
| `name` | `WireStationVPN` (unique in the catalog, ignoring case) |
| `kind` | `app` |
| `category` | `utilities` |
| `description` | 162 characters (the cap is 200), written by the workflow |
| `license` | `GPL-3.0` (GitHub detects the root LICENSE) |
| `author` | `ThatRetiredDude` |
| `version` | the tag |
| `artifact_url`, `sha256` | the release's ZIP |
| `icon_url` | `app/sce_sys/icon0.png` at the tag (512x512) |

## Rules this app keeps

- **One title ID forever.** titleId `PPSA99556`, conceptId `99556`,
  contentId `UP9000-PPSA99556_00-WIRESTATIONVPN00` never change, or updates
  stop being updates. PPSA99556 is in the homebrew range; re-check the
  catalog for it before the first listing.
- **`downloadDataSize` stays 256.** Higher made TokenStation5 refuse to
  start (CE-107694-7).
- **User data is outside the app folder,** which a store update replaces:
  settings, the PS5's Tailscale keys, logs and received files are in
  `/data/wirestation/`, so an update or a reinstall keeps them and the PS5
  stays signed in.
- **After a PS5 restart** the jailbreak has to run again before the app
  opens, and the background part starts when the app does. The About page
  says so.
