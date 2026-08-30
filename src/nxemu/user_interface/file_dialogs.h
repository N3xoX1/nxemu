#pragma once

class Path;

bool FileSelect(void * hwndOwner, const char * initialDir, const char * fileFilter, bool fileMustExist, Path & selected);
Path BrowseForDirectory(void * parentWindow, const char * title);
