#ifndef BACKUPHELPER_H
#define BACKUPHELPER_H

#include <c++utilities/application/global.h>

#include <string>
#include <fstream>

namespace Media {

namespace BackupHelper {

LIB_EXPORT std::string &backupDirectory();
LIB_EXPORT void restoreOriginalFileFromBackupFile(const std::string &originalPath, const std::string &backupPath, std::fstream &originalStream, std::fstream &backupStream);
LIB_EXPORT void createBackupFile(const std::string &originalPath, std::string &backupPath, std::fstream &backupStream);

}

}

#endif // BACKUPHELPER_H