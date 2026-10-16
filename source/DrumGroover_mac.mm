// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
// macOS NSSavePanel/NSOpenPanel implementation of PlatformFileDialog.h —
// see that header for the full contract. Only compiled on Mac (see
// CMakeLists.txt's `if(APPLE)` source guard); OS_MAC is also checked here
// so this file is inert (compiles to nothing) if it ever gets included in
// a non-Mac build by mistake.
#ifdef OS_MAC

#include "PlatformFileDialog.h"
#import <Cocoa/Cocoa.h>

// allowedFileTypes (extension-string based) is deprecated in favor of
// allowedContentTypes (UTType-based) since macOS 11, but still works on
// every currently shipping macOS version and doesn't require picking a
// minimum deployment target for UTType — matches this project's
// not-yet-established macOS deployment target. Revisit once that's pinned
// down for a real Mac build.

std::string MacSaveFileDialog(void* nsView, const char* extension, const char* typeDescription, const std::string& suggestedFilename)
{
  (void)nsView; // see PlatformFileDialog.h — unused for now, runModal doesn't need an owning window
  @autoreleasepool
  {
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.allowedFileTypes = @[ [NSString stringWithUTF8String:extension] ];
    panel.allowsOtherFileTypes = NO;
    panel.canCreateDirectories = YES;
    panel.nameFieldStringValue = [NSString stringWithUTF8String:suggestedFilename.c_str()];
    if (typeDescription) panel.message = [NSString stringWithUTF8String:typeDescription];

    if ([panel runModal] == NSModalResponseOK)
      return std::string([[[panel URL] path] UTF8String]);

    return ""; // cancelled — same convention Win32SaveFileDialog uses
  }
}

std::string MacOpenFileDialog(void* nsView, const char* extension, const char* typeDescription)
{
  (void)nsView;
  @autoreleasepool
  {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.allowedFileTypes = @[ [NSString stringWithUTF8String:extension] ];
    panel.allowsOtherFileTypes = NO;
    panel.allowsMultipleSelection = NO;
    panel.canChooseDirectories = NO;
    panel.canChooseFiles = YES;
    if (typeDescription) panel.message = [NSString stringWithUTF8String:typeDescription];

    if ([panel runModal] == NSModalResponseOK)
    {
      NSURL* url = [[panel URLs] firstObject];
      if (url) return std::string([[url path] UTF8String]);
    }
    return ""; // cancelled — same convention Win32OpenFileDialog uses
  }
}

#endif // OS_MAC
