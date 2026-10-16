#pragma once
#include <initializer_list>
#include <string>
#include <utility>

class Path;

// The kinds of files a file dialog offers, as FileSelect() takes them: for
// each, its name and its patterns, "*.png;*.jpg".
std::string FileFilter(std::initializer_list<std::pair<std::string, const char *>> kinds);
bool FileSelect(void * hwndOwner, const char * initialDir, const char * fileFilter, bool fileMustExist, Path & selected);
Path BrowseForDirectory(void * parentWindow, const char * title);
