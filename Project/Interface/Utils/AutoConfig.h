#pragma once

/** True if auto_config.ini existed when the app started. */
bool AutoConfig_HadFileOnLoad();

/** Load auto_config.ini from the exe directory (menu + KmBox + debug). */
void AutoConfig_Load();

/** Call once per frame; saves after settings stop changing (~0.5s). */
void AutoConfig_Tick();

/** Flush settings to disk immediately (exit / shutdown). */
void AutoConfig_SaveNow();

/** Schedule a debounced save (KmBox UI, etc.). */
void AutoConfig_MarkDirty();
