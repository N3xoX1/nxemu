#pragma once
#include <string>

// Opens a web address in the user's browser.
bool OpenUrl(const std::string & url);
// Opens a directory in the system's file manager.
bool OpenDirectory(const std::string & utf8Path);
