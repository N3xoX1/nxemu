#pragma once
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

class Path;

std::string FileFilter(std::initializer_list<std::pair<std::string, const char *>> kinds);
bool FileSelect(void * hwndOwner, const char * initialDir, const char * fileFilter, bool fileMustExist, Path & selected);
// Several existing files, as UTF-8 paths. Where the system picker cannot
// select several, it returns the one file chosen.
bool FileSelectMany(void * hwndOwner, const char * initialDir, const char * fileFilter, std::vector<std::string> & selected);
bool FileSave(void * hwndOwner,const char * initialDir,const char * fileFilter,const char * suggestedName,Path & selected);
Path BrowseForDirectory(void * parentWindow, const char * title);
bool MoveFileToTrash(void * parentWindow, const std::string & path);
