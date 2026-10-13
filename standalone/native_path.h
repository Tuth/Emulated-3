// Paths returned by the Win32 A file dialog and used by fopen are native code-page bytes.
// Split separators without asking std::filesystem to decode them as UTF-8.
#pragma once
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <filesystem>
#endif
namespace eiii {
inline std::string path_leaf(std::string const &path) {
    auto p=path.find_last_of("/\\");
    return p==std::string::npos?path:path.substr(p+1);
}
inline std::string path_parent(std::string const &path) {
    auto p=path.find_last_of("/\\");
    if(p==std::string::npos)return ".";
    if(p==0 || (p==2 && path[1]==':'))return path.substr(0,p+1);
    return path.substr(0,p);
}
inline bool create_native_directory(std::string const &path) {
#ifdef _WIN32
    return CreateDirectoryA(path.c_str(),nullptr) || GetLastError()==ERROR_ALREADY_EXISTS;
#else
    std::error_code error;
    std::filesystem::create_directories(path,error);
    return !error;
#endif
}
// The software renderer expects UTF-8; convert for presentation, never for file access.
inline std::string native_display(std::string const &text) {
#ifdef _WIN32
    if(text.empty())return {};
    int n=MultiByteToWideChar(CP_ACP,0,text.data(),int(text.size()),nullptr,0);
    if(!n)return text;
    std::wstring wide(n,L'\0');
    if(!MultiByteToWideChar(CP_ACP,0,text.data(),int(text.size()),wide.data(),n))return text;
    int m=WideCharToMultiByte(CP_UTF8,0,wide.data(),n,nullptr,0,nullptr,nullptr);
    if(!m)return text;
    std::string utf8(m,'\0');
    if(!WideCharToMultiByte(CP_UTF8,0,wide.data(),n,utf8.data(),m,nullptr,nullptr))return text;
    return utf8;
#else
    return text;
#endif
}
}
