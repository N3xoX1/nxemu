#pragma once
#include <initializer_list>
#include <string>
#include <utility>

class Path;

std::string FileFilter(std::initializer_list<std::pair<std::string, const char *>> kinds);
bool FileSelect(void * hwndOwner, const char * initialDir, const char * fileFilter, bool fileMustExist, Path & selected);
bool FileSave(void * hwndOwner,const char * initialDir,const char * fileFilter,const char * suggestedName,Path & selected);
Path BrowseForDirectory(void * parentWindow, const char * title);
