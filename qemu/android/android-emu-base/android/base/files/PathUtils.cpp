// Copyright 2014 The Android Open Source Project
//
// This software is licensed under the terms of the GNU General Public
// License version 2, as published by the Free Software Foundation, and
// may be copied, distributed, and modified under those terms.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

#include "aemu/base/files/PathUtils.h"

#if defined(__linux__) || defined(_WIN32) ||       \
        (defined(MAC_OS_X_VERSION_MIN_REQUIRED) && \
         MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_5)
#define _SUPPORT_FILESYSTEM
#endif

#ifdef _SUPPORT_FILESYSTEM
#include <filesystem>
#endif                                   //_SUPPORT_FILESYSTEM
#include <errno.h>
#include <string.h>                      // for size_t, strncmp
#include <iterator>                      // for reverse_iterator, operator!=
#include <numeric>                       // for accumulate
#include <string_view>
#include <type_traits>                   // for enable_if<>::type

#include "android/base/system/System.h"  // for System

namespace android {
namespace base {

const char* const PathUtils::kExeNameSuffixes[kHostTypeCount] = {"", ".exe"};

const char* const PathUtils::kExeNameSuffix =
        PathUtils::kExeNameSuffixes[PathUtils::HOST_TYPE];

std::string PathUtils::toExecutableName(const char* baseName,
                                        HostType hostType) {
    return static_cast<std::string>(baseName).append(
            kExeNameSuffixes[hostType]);
}

// static
bool PathUtils::isDirSeparator(int ch, HostType hostType) {
    return (ch == '/') || (hostType == HOST_WIN32 && ch == '\\');
}

// static
bool PathUtils::isPathSeparator(int ch, HostType hostType) {
    return (hostType == HOST_POSIX && ch == ':') ||
           (hostType == HOST_WIN32 && ch == ';');
}

// static
size_t PathUtils::rootPrefixSize(const std::string& path, HostType hostType) {
    if (path.empty())
        return 0;

    if (hostType != HOST_WIN32)
        return (path[0] == '/') ? 1U : 0U;

    size_t result = 0;
    if (path[1] == ':') {
        int ch = path[0];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z'))
            result = 2U;
    } else if (!strncmp(path.c_str(), "\\\\.\\", 4) ||
               !strncmp(path.c_str(), "\\\\?\\", 4)) {
        // UNC prefixes.
        return 4U;
    } else if (isDirSeparator(path[0], hostType)) {
        result = 1;
        if (isDirSeparator(path[1], hostType)) {
            result = 2;
            while (path[result] && !isDirSeparator(path[result], HOST_WIN32))
                result++;
        }
    }
    if (result && path[result] && isDirSeparator(path[result], HOST_WIN32))
        result++;

    return result;
}

// static
bool PathUtils::isAbsolute(const char* path, HostType hostType) {
    size_t prefixSize = rootPrefixSize(path, hostType);
    if (!prefixSize) {
        return false;
    }
    if (hostType != HOST_WIN32) {
        return true;
    }
    return isDirSeparator(path[prefixSize - 1], HOST_WIN32);
}

std::string_view PathUtils::extension(const char* path,
                                      HostType hostType) {
    std::string_view tmp = path;
    using riter = std::reverse_iterator<std::string_view::const_iterator>;

    for (auto it = riter(tmp.end()), itEnd = riter(tmp.begin()); it != itEnd;
         ++it) {
        if (*it == '.') {
            // reverse iterator stores a base+1, so decrement it when returning
            // MSVC doesn't have string_view constructor with iterators
            return std::string_view(&*std::prev(it.base()), (it - riter(tmp.end()) + 1));
        }
        if (isDirSeparator(*it, hostType)) {
            // no extension here - we've found the end of file name already
            break;
        }
    }

    // either there's no dot in the whole path, or we found directory separator
    // first - anyway, there's no extension in this name
    return "";
}

// static
std::string_view PathUtils::extension(const std::string& path,
                                      HostType hostType) {
    return PathUtils::extension(path.c_str(), hostType);
}

// static
std::string PathUtils::removeTrailingDirSeparator(const char* path,
                                                  HostType hostType) {
    size_t pathLen = strlen(path);
    // NOTE: Don't remove initial path separator for absolute paths.
    while (pathLen > 1U && isDirSeparator(path[pathLen - 1U], hostType)) {
        pathLen--;
    }
    return std::string(path, pathLen);
}

// static
std::string PathUtils::addTrailingDirSeparator(const char* path,
                                               HostType hostType) {
    std::string result(path);
    if (result.size() > 0 && !isDirSeparator(result[result.size() - 1U])) {
        result += getDirSeparator(hostType);
    }
    return result;
}

// static
std::string PathUtils::addTrailingDirSeparator(const std::string& path, HostType hostType) {
    return addTrailingDirSeparator(path.c_str(), hostType);
}

// static
bool PathUtils::split(const char* path,
                      HostType hostType,
                      std::string* dirName,
                      std::string* baseName) {
    if (!path || path[0] == '\0') return false;

    // If there is a trailing directory separator, return an error.
    size_t end = strlen(path);
    if (isDirSeparator(path[end - 1U], hostType)) {
        return false;
    }

    // Find last separator.
    size_t prefixLen = rootPrefixSize(path, hostType);
    size_t pos = end;
    while (pos > prefixLen && !isDirSeparator(path[pos - 1U], hostType)) {
        pos--;
    }

    // Handle common case.
    if (pos > prefixLen) {
        if (dirName) {
            *dirName = std::string(path, pos);
        }
        if (baseName) {
            *baseName = path + pos;
        }
        return true;
    }

    // If there is no directory separator, the path is a single file name.
    if (dirName) {
        if (!prefixLen) {
            *dirName = ".";
        } else {
            *dirName = std::string(path, prefixLen);
        }
    }
    if (baseName) {
        *baseName = path + prefixLen;
    }
    return true;
}

// static
std::string PathUtils::join(const std::string& path1,
                            const std::string& path2,
                            HostType hostType) {
    if (path1.empty()) {
        return path2.c_str();
    }
    if (path2.empty()) {
        return path1.c_str();
    }
    if (isAbsolute(path2.c_str(), hostType)) {
        return path2.c_str();
    }
    size_t prefixLen = rootPrefixSize(path1, hostType);
    std::string result(path1);
    size_t end = result.size();
    if (end > prefixLen && !isDirSeparator(result[end - 1U], hostType)) {
        result += getDirSeparator(hostType);
    }
    result += path2;
    return result;
}

template <class String>
static std::vector<String> decomposeImpl(const String& path, PathUtils::HostType hostType) {
    std::vector<String> result;
    if (path.empty())
        return result;

    size_t prefixLen = PathUtils::rootPrefixSize(path.c_str(), hostType);
    auto it = path.begin();
    if (prefixLen) {
        result.emplace_back(&*it, prefixLen);
        it += prefixLen;
    }
    for (;;) {
        auto p = it;
        while (p != path.end() && !PathUtils::isDirSeparator(*p, hostType))
            p++;
        if (p > it) {
            result.emplace_back(&*it, (p - it));
        }
        if (p == path.end()) {
            break;
        }
        it = p + 1;
    }
    return result;
}

std::vector<std::string> PathUtils::decompose(std::string&& path,
                                              HostType hostType) {
    return decomposeImpl<std::string>(path, hostType);
}

// static
std::vector<std::string> PathUtils::decompose(const std::string& path,
                                              HostType hostType) {
    return decomposeImpl<std::string>(path, hostType);
}

template <class String>
std::string PathUtils::recompose(const std::vector<String>& components,
                                 HostType hostType) {
    if (components.empty()) {
        return {};
    }

    const char dirSeparator = getDirSeparator(hostType);
    std::string result;
    // To reduce memory allocations, compute capacity before doing the
    // real append.
    const size_t capacity =
            components.size() - 1 +
            std::accumulate(components.begin(), components.end(), size_t(0),
                            [](size_t val, const String& next) {
                                return val + next.size();
                            });

    result.reserve(capacity);
    bool addSeparator = false;
    for (size_t n = 0; n < components.size(); ++n) {
        const auto& component = components[n];
        if (addSeparator)
            result += dirSeparator;
        addSeparator = true;
        if (n == 0) {
            size_t prefixLen = rootPrefixSize(std::string(component.begin(), component.end()),
                                              hostType);
            if (prefixLen == component.size()) {
                addSeparator = false;
            }
        }
        result += component;
    }
    return result;
}

// static
std::string PathUtils::recompose(const std::vector<std::string>& components,
                                 HostType hostType) {
    return recompose<std::string>(components, hostType);
}

// static
template <class String>
void PathUtils::simplifyComponents(std::vector<String>* components) {
    std::vector<String> stack;
    for (auto& component : *components) {
        if (component == std::string_view(".")) {
            // Ignore any instance of '.' from the list.
            continue;
        }
        if (component == std::string_view("..")) {
            // Handling of '..' is specific: if there is a item on the
            // stack that is not '..', then remove it, otherwise push
            // the '..'.
            if (!stack.empty() && stack.back() != std::string_view("..")) {
                stack.pop_back();
            } else {
                stack.push_back(std::move(component));
            }
            continue;
        }
        // If not a '..', just push on the stack.
        stack.push_back(std::move(component));
    }
    if (stack.empty()) {
        stack.push_back(".");
    }
    components->swap(stack);
}

void PathUtils::simplifyComponents(std::vector<std::string>* components) {
    simplifyComponents<std::string>(components);
}

std::string PathUtils::canonicalPath(std::string desired_directory) {
    auto path = PathUtils::decompose(desired_directory);
    PathUtils::simplifyComponents(&path);
    auto recomposed = PathUtils::recompose(path);
    return recomposed;
}

// static
std::string PathUtils::relativeTo(const std::string& base,
                                  const std::string& path,
                                  HostType hostType) {
    auto baseDecomposed = decompose(base, hostType);
    auto pathDecomposed = decompose(path, hostType);

    if (baseDecomposed.size() > pathDecomposed.size())
        return path;

    for (size_t i = 0; i < baseDecomposed.size(); i++) {
        if (baseDecomposed[i] != pathDecomposed[i])
            return path;
    }

    std::string result =
            recompose(std::vector<std::string_view>(
                              pathDecomposed.begin() + baseDecomposed.size(),
                              pathDecomposed.end()),
                      hostType);

    return result;
}

// static
Optional<std::string> PathUtils::pathWithoutDirs(const char* name) {
    if (System::get()->pathIsDir(name))
        return kNullopt;

    auto components = PathUtils::decompose(name);

    if (components.empty())
        return kNullopt;

    return std::string(components.back());
}

Optional<std::string> PathUtils::pathToDir(const char* name) {
    if (System::get()->pathIsDir(name))
        return std::string(name);

    auto components = PathUtils::decompose(name);

    if (components.size() == 1)
        return kNullopt;

    return PathUtils::recompose(std::vector<std::string_view>(
            components.begin(), components.end() - 1));
}

std::string pj(const std::string& path1, const std::string& path2) {
    return PathUtils::join(path1, path2);
}

std::string pj(const std::vector<std::string>& paths) {
    std::string res;

    if (paths.size() == 0)
        return "";

    if (paths.size() == 1)
        return paths[0];

    res = paths[0];

    for (size_t i = 1; i < paths.size(); i++) {
        res = pj(res, paths[i]);
    }

    return res;
}

Optional<std::string> PathUtils::pathWithEnvSubstituted(const char* path) {
    return PathUtils::pathWithEnvSubstituted(PathUtils::decompose(std::string(path)));
}

Optional<std::string> PathUtils::pathWithEnvSubstituted(
        std::vector<std::string> decomposedPath) {
    std::vector<std::string> dest;
    for (auto component : decomposedPath) {
        if (component.size() > 3 && component[0] == '$' &&
            component[1] == '{' && component[component.size() - 1] == '}') {
            int len = component.size();
            auto var = component.substr(2, len - 3);
            auto val = System::get()->envGet(var);
            if (val.empty()) {
                return {};
            }
            dest.push_back(val);
        } else {
            dest.emplace_back(component);
        }
    }

    return PathUtils::recompose(dest);
}

bool PathUtils::move(const std::string& from, const std::string& to) {
    // std::rename returns 0 on success.
    if (std::rename(from.c_str(), to.c_str())) {
        if (errno == ENOENT) {
            return false;
        }

#ifdef _SUPPORT_FILESYSTEM
        // Rename can fail if files are on different disks
        if (std::filesystem::copy_file(from.c_str(), to.c_str())) {
            std::filesystem::remove(from.c_str());
            return true;
        } else {
            return false;
        }
#else   // _SUPPORT_FILESYSTEM
        return false;
#endif  // _SUPPORT_FILESYSTEM
    }
    return true;
}

}  // namespace base
}  // namespace android
