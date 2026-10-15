#pragma once
#include <string>

// Native macOS Save/Open panels (NSSavePanel/NSOpenPanel).
// nsView: WebViewEditorDelegate::mView (void*) — currently unused, kept for parity with Win32.
// extension: file extension without leading dot (e.g., "mid").
// typeDescription: panel prompt text (e.g., "MIDI File").
// suggestedFilename: pre-filled name for save dialog.
// Returns the chosen path (UTF-8), or empty string if cancelled.
#ifdef OS_MAC
std::string MacSaveFileDialog(void* nsView, const char* extension, const char* typeDescription, const std::string& suggestedFilename);
std::string MacOpenFileDialog(void* nsView, const char* extension, const char* typeDescription);
#endif