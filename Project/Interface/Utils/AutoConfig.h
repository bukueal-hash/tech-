#pragma once

#include <string>
#include <vector>

/** True if auto_config.ini existed when the app started. */
bool AutoConfig_HadFileOnLoad();

/** Load auto_config.ini from the exe directory (menu + KmBox + debug). */
void AutoConfig_Load();

/** Call once per frame; saves after settings stop changing (~0.5s). */
void AutoConfig_Tick();

/** Flush settings to disk immediately (exit / shutdown). */
void AutoConfig_SaveNow();

/** Flush settings to disk without blocking the caller on file I/O (paint-safe). */
void AutoConfig_SaveNowAsync();

/** Schedule a debounced save (KmBox UI, etc.). */
void AutoConfig_MarkDirty();

// D20 — baseline, diff, export, apply.
//
// A baseline is a frozen copy of auto_config.ini; the diff compares it against
// a freshly flushed copy of the live settings. Both live in Data\ beside the
// exe so they survive restarts and can be shared. Export/apply work the same
// way, which means a hand-edited export is a valid way to ship settings.

/** Freeze the live settings into Data\config_baseline.ini. */
bool AutoConfig_SnapshotBaseline();

/** Differences between the baseline and the live settings, one readable line
 *  per key: "key: old -> new", "+ key (new)" or "- key (removed, was old)".
 *  Empty when there is no baseline or the two match. Has no side effects — the
 *  caller flushes first if it wants the freshest state. */
std::vector<std::string> AutoConfig_DiffBaseline();

/** Write the live settings to Data\config_export.ini. */
bool AutoConfig_ExportCurrent();

/** Load Data\config_export.ini over the live settings and schedule a save.
 *  False when the file is missing or holds no known keys. */
bool AutoConfig_ImportExported();

/** Paths shown in the menu so the user knows which file each button touches. */
std::string AutoConfig_BaselinePath();
std::string AutoConfig_ExportPath();
