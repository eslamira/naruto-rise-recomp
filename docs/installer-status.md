# Installer verification

[Documentation](README.md) · [Packaging](../packaging/README.md)

This document lists available tests and the verification workflow. The existence of tests does not mean they have passed on the current build; record results for each release.

## Existing tests

| File | Scope |
| --- | --- |
| `packaging/tests/test_game_paths.cpp` | Launcher game data discovery |
| `packaging/tests/test_setup_helper.py` | XEX/Title ID validation, synthetic ISOs, and extraction helper |
| `packaging/tests/test_setup_inno.py` | Installation, updates between releases, legacy/portable migration, version checks, and uninstall preservation in temporary directories |
| `packaging/tests/test_dlc_runtime.py` | Opt-in startup test of the base game and each character DLC engine; needs user-supplied game files and content ([DLCs](dlc.md#6-startup-regression-test)) |
| `launcher/tests/test_config_manager.cpp` | Independent game/interface language persistence and configuration compatibility |
| `launcher/tests/test_localization.cpp` | Seven complete translations, language order, codes, and formatting placeholders |
| `launcher/tests/test_update_ui.cpp` + `packaging/tests/validate_launcher_update.py` | Real launcher dialogs, download verification, real Inno update, restart and preservation under a private test AppId |

After building the required binaries, run from the repository root:

```powershell
cmake --build app/out/build/win-amd64-release --target narutorise_game_paths_test narutorise_setup_helper
.\app\out\build\win-amd64-release\narutorise_game_paths_test.exe
python .\packaging\tests\test_setup_helper.py -v
```

For integration tests, prepare a distribution and make Inno Setup available:

```powershell
$env:NARUTORISE_TEST_PACKAGE = (Resolve-Path .\dist\naruto-rise-recomp-win-amd64).Path
python .\packaging\tests\test_setup_inno.py -v
```

See the test sources for requirements and compiler paths. ISO fixtures are synthetic; integration tests use a dedicated AppId and temporary installations.

## Manual verification for each release

- Install under a standard user account and launch through the shortcut.
- Import a compatible ISO, including a path with spaces or accented characters.
- Cancel an import and verify that no partial `game/` directory was published.
- Reinstall while preserving existing data and preferences.
- Update an Inno, legacy native, and portable installation through **Update an existing installation**, without importing an ISO again.
- Confirm the folder picker selects the existing folder directly, the ISO page is skipped, the ready summary shows versions, and completion reports an update.
- Reject updates to a folder missing the port executables and reject older numeric release versions without changing existing files.
- Run the portable package and select an extracted game folder.
- Verify the installer starts in English and offers English, Portuguese, French, German, Spanish, Italian, and Russian in that order.
- Switch all seven launcher languages, including Cyrillic text, graphics settings, and 16:9/ultrawide aspect ratios.
- Confirm the bundled **Português (Brasil)** translation appears alongside the five native game languages on new installations and updates, without changing the saved selection. Interface language remains independent.
- Update an older bundled translation and check that other mod packages and personal files remain intact.
- Import DLC and launch a build containing `narutorise_ai2c2.dll`.
- For builds with the native renderer: launch once with each renderer, check that the options of the other renderer are greyed out, and play with the frame rate at 30, 60, and 120.
- Check saves, logs, and cache in the documented locations.
- Uninstall and confirm user data preservation.

Record the version, environment, command/procedure, result, and log. Real ISO compatibility and the installer's visual behavior require manual verification in addition to automated fixtures.

## Launcher update validation — 2026-10-20

The complete launcher-to-Setup workflow passed for both registered and portable
copies, updating `0.2.0-beta.1` to `0.2.0-beta.2` in an isolated directory whose
name contains spaces and Portuguese accents. No release was generated or
published. Local fixture installers use the real `installer.iss` code with a
unique test AppId, a private destination and no shortcut entries. Their game
binaries and user data are synthetic; the updated launcher is the actual build.

The opt-in UI test includes the production launcher's main loop, rendering,
confirmation dialogs and installer handoff. It injects input into ImGui only,
without sending input to the desktop. Its download services stream the local
fixture through the production size/hash/file-lock pipeline. The registered
case redirects HKCU only inside the test launcher process; Setup independently
uses its private test AppId. Production has no UI or command-line switch for
these fixture services, and the test executable is excluded from normal builds.

Verified in both runs:

- **No** on the first confirmation starts no transfer; **Yes** starts one.
- The second confirmation appears only after verification; **No** keeps the
  launcher running and re-opening it does not download the file again.
- The second **Yes** saves configuration and closes the launcher after Setup
  starts. The wizard receives the correct destination and Portuguese language.
- Setup completes the update and reopens the updated launcher. Version marker,
  registry version, replacement binaries and bundled translation match the fixture.
- Configuration values survive the whole flow; Setup preserves the saved TOML
  byte-for-byte. Eleven sentinel files covering game folders, saves, installed
  and pending DLC, community mods, logs and personal files survive update and
  uninstall. Test registrations are removed afterward.
- The production registration and hashes of the executables, configuration and
  version marker in `C:\games\Naruto` remain unchanged.

The first run exposed a pre-existing Unicode path defect: the launcher supplied
an ANSI path to the TOML parser, failed to load settings in an accented folder,
then saved defaults. Configuration now opens `std::filesystem::path` directly
through a stream; the regression test includes Portuguese and Japanese path
characters and unmanaged keys. Font and cover paths now use UTF-8 with their
respective readers. The configuration regression and both complete update runs
passed after these fixes.

GitHub transport was checked separately with
`narutorise_update_installer_test.exe --live`: the real published installer
(44,516,764 bytes) downloaded and passed SHA-256 verification without execution.
The public installer itself was not installed into the user's real game folder.

Reproduce from the VS x64 development environment:

```powershell
cmake --build app/out/build/win-amd64-release --target narutorise_launcher narutorise_update_ui_test narutorise_config_manager_test
& app/out/build/win-amd64-release/narutorise_config_manager_test.exe out/update-config-regression
python packaging/tests/validate_launcher_update.py
python packaging/tests/validate_launcher_update.py --portable
```

Local evidence (JSON results, launcher events, wizard pages and cleanup checks):

- Registered: `out/update-validation-20261020-181708-32adca/`.
- Portable: `out/update-validation-20261020-181748-a29af1/`.

These fixtures validate updating and preserving data, not gameplay or real ISO
compatibility. Test packages and logs remain under `out/` for inspection.

## Update verification — 2026-10-08

Inno Setup 6.7.3 compiled the manual update installer. All 17 integration tests
passed on Windows using the existing helper build at `out/language-check/build`
and the synthetic package at `out/language-check/installer-fixture`:

```powershell
$env:NARUTORISE_TEST_BUILD = (Resolve-Path .\out\language-check\build).Path
$env:NARUTORISE_TEST_PACKAGE = (Resolve-Path .\out\language-check\installer-fixture).Path
python .\packaging\tests\test_setup_inno.py -v
```

The suite compiled versions 1.0.0 and 1.0.1 under one unique test AppId, verified
binary replacement and the updated registration, and checked byte-for-byte
preservation of configuration, original game files, pending/installed DLCs,
saves, logs and personal files through update and uninstall. Legacy custom-path
detection, unknown-version portable migration, repeated updates, explicit ISO
import in install mode, and rejection of invalid destinations/modes, update ISO
imports, and older numeric releases passed. The game was never launched and
these synthetic packages are not distributable game builds. Interactive wizard
layout and real game launch remain manual checks.

The local distributable `dist/NarutoRiseInstaller.exe` was assembled as version
1.0.1 (17,506,758 bytes). Its game/runtime binaries came from the existing 1.0.0
package in `C:\Users\Jorge\dev\naruto-rise-recomp\dist\naruto-rise-recomp-win-amd64`;
the launcher/helper Release build, configuration, assets and installer came from
this checkout. PE import checks rejected debug CRT dependencies. The assembly
recipe and source binary hashes are recorded locally in
`out/manual-update-check/assemble-release.ps1` and `release-provenance.json`.
The installer SHA-256 is
`B180DBFA063ECFEA55464780DD0D571955168B3F7B8C88D2EC7143550257DF65`.

All 17 integration tests also passed against that assembled real package in
119.987 seconds, with `NARUTORISE_TEST_PACKAGE` pointing to
`dist/naruto-rise-recomp-win-amd64`. This run used unique GUID AppIds with the
same brace escaping as production. Install/update/uninstall were exercised in
temporary destinations; the production registration and game were not touched.

## Beta 1 migration verification — 2026-10-11

Issue #11 reports beta 1's internal `1.0.0-beta.1` version being treated as
newer than `0.1.0-beta.2`. Before the fix, the integration test using that
exact previous version failed with installer exit code 7. The beta 2 migration
allowlist now includes `1.0.0-beta.1` alongside the existing old labels.

All 17 Inno integration tests passed in 66.245 seconds using
`out/language-check/installer-fixture` and the helper from
`app/out/build/win-amd64-release`. The tests use unique test AppIds and temporary
destinations. Coverage includes beta 1 metadata read from the version file,
Inno registration and legacy registration; preservation of configuration,
game data, DLCs, saves and personal files; and rejection of the reverse
transition and unrelated newer versions (`1.0.0-beta.2`, `1.0.0-beta.10`,
`1.0.10`). The game was not launched.

The three update, version-check and downgrade tests also passed against the
actual beta 2 package in `dist/v0.1.0-beta.2-update/naruto-rise-recomp-win-amd64`
in 104.793 seconds, with isolated test AppIds. That directory's original
installer hash matches the GitHub beta 2 release asset. A corrected installer
was compiled locally from the same package into
`dist/issue-11-fix/NarutoRiseInstaller.exe` (20,325,174 bytes), with SHA-256
`065690add96792cc27ab2826d60a4468d3d4f7a29f22160c685352d23eabf11a`.
It has not been published; interactive wizard validation remains a manual check.

## Beta 3 artifact preparation — 2026-10-11

Local release files are in `dist/v0.1.0-beta.3/`. This is an installer and
metadata update using the verified published beta 2 portable payload from
`dist/v0.1.0-beta.2-update/`. The game, launcher, runtime and all three character
DLC engine DLLs retain their beta 2 hashes. Unrelated local native-renderer
experiments are excluded; no game/runtime rebuild was performed.

The installer product version and portable marker both read `0.1.0-beta.3`.
The numbering-reset exception covers beta 3 as well as beta 2, allowing beta 1's
`1.0.0-beta.1` identifier and the old local labels to migrate. The integration
suite also covers beta 2 metadata found only in the Inno registration.

The assembly recipe is `out/release-v0.1.0-beta.3/assemble.py`. Binary import
checks found no debug CRT dependencies. Every file in the ZIP matches its
portable source, and `game/` contains only the distribution placeholder.
No GitHub release or tag was created. Gameplay and the interactive wizard
remain outside this verification scope.

All 18 installer integration tests passed against the beta 3 portable package
in 196.908 seconds, using unique test AppIds and temporary destinations.
The beta 1 migration and beta 2 registry-only update both preserved user files.
The test log, release notes, source patch, build provenance and SHA-256 checksums
are stored alongside the local installer and ZIP.
