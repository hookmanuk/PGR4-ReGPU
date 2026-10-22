// The user folder (saves, settings, caches, title update, DLC): Documents\pgr4, in the Documents folder
// Windows reports (a moved Documents folder, E:\Documents, isn't %USERPROFILE%\Documents).
#include "user_dir.h"

#include <windows.h>

#include <shlobj.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

const std::filesystem::path& DocumentsUserDir()
{
    static const std::filesystem::path dir = [] {
        PWSTR known = nullptr;
        std::filesystem::path documents;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &known)) && known)
            documents = known;
        CoTaskMemFree(known);
        return documents.empty() ? std::filesystem::path("user") : documents / "pgr4";
    }();
    return dir;
}
