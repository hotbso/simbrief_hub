//
//    Copyright (C) 2025  Holger Teutsch
//
//    This library is free software; you can redistribute it and/or
//    modify it under the terms of the GNU Lesser General Public
//    License as published by the Free Software Foundation; either
//    version 2.1 of the License, or (at your option) any later version.
//
//    This library is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
//    Lesser General Public License for more details.
//
//    You should have received a copy of the GNU Lesser General Public
//    License along with this library; if not, write to the Free Software
//    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301
//    USA
//

#include <cstdlib>
#include <cstdio>

#include "http_get.h"
#include "log_msg.h"

#if IBM == 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <WinHttp.h>
bool HttpGetInitialize() {
    return true;
}

void HttpGetFinalize() {
}

bool
HttpGet(const std::string& url, std::string& data, int timeout)
{
    data.clear();

    DWORD dwSize = 0;
    DWORD dwDownloaded = 0;
    BOOL  bResults = FALSE;
    HINTERNET  hSession = NULL,
               hConnect = NULL,
               hRequest = NULL;

    int result = false;

    int url_len = url.length();
    WCHAR *url_wc = (WCHAR *)alloca((url_len + 1) * sizeof(WCHAR));
    WCHAR *host_wc = (WCHAR *)alloca((url_len + 1) * sizeof(WCHAR));
    WCHAR *path_wc = (WCHAR *)alloca((url_len + 1) * sizeof(WCHAR));

    mbstowcs_s(NULL, url_wc, url_len + 1, url.c_str(), _TRUNCATE);

    URL_COMPONENTS urlComp;
    memset(&urlComp, 0, sizeof(urlComp));
    urlComp.dwStructSize = sizeof(urlComp);

    urlComp.lpszHostName = host_wc;
    urlComp.dwHostNameLength  = (DWORD)(url_len + 1);

    urlComp.lpszUrlPath = path_wc;
    urlComp.dwUrlPathLength   = (DWORD)(url_len + 1);

    // Crack the url_wc.
    if (!WinHttpCrackUrl(url_wc, 0, 0, &urlComp)) {
        LogMsg("Error in WinHttpCrackUrl: %lu", GetLastError());
        goto error_out;
    }

    char buffer[16 * 1024];

    // Use WinHttpOpen to obtain a session handle.
    hSession = WinHttpOpen( L"sbh",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0 );

    if (NULL == hSession) {
        LogMsg("Can't open HTTP session");
        goto error_out;
    }

    timeout *= 1000;
    if (! WinHttpSetTimeouts(hSession, timeout, timeout, timeout, timeout)) {
        LogMsg("can't set timeouts");
        goto error_out;
    }

    hConnect = WinHttpConnect(hSession, host_wc, urlComp.nPort, 0);
    if (NULL == hConnect) {
        LogMsg("Can't open HTTP session");
        goto error_out;
    }

    hRequest = WinHttpOpenRequest(hConnect, L"GET", path_wc, NULL, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES,
                                  (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0);
    if (NULL == hRequest) {
        LogMsg("Can't open HTTP request: %lu", GetLastError());
        goto error_out;
    }

    bResults = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (! bResults) {
        LogMsg("Can't send HTTP request: %lu", GetLastError());
        goto error_out;
    }

    bResults = WinHttpReceiveResponse(hRequest, NULL);
    if (! bResults) {
        LogMsg("Can't receive response: %lu", GetLastError());
        goto error_out;
    }

    while (1) {
        DWORD res = WinHttpQueryDataAvailable(hRequest, &dwSize);
        if (!res) {
            LogMsg("%lu, Error %lu in WinHttpQueryDataAvailable.", res, GetLastError());
            goto error_out;
        }

        // LogMsg("dwSize %d", dwSize);
        if (0 == dwSize) {
            break;
        }

        while (dwSize > 0) {
            int get_len = (dwSize < sizeof(buffer) ? dwSize : sizeof(buffer));

            bResults = WinHttpReadData(hRequest, buffer, get_len, &dwDownloaded);
            if (! bResults){
               LogMsg("Error %lu in WinHttpReadData.", GetLastError());
               goto error_out;
            }

            data.append(buffer, dwDownloaded);
            dwSize -= dwDownloaded;
        }
    }

    result = true;

error_out:
    // Close any open handles.
    if (hRequest) WinHttpCloseHandle(hRequest);
    if (hConnect) WinHttpCloseHandle(hConnect);
    if (hSession) WinHttpCloseHandle(hSession);

    //LogMsg("HttpGet result: %d", result);
    return result;
}

#else   // Linux or MacOS
#include <curl/curl.h>

#ifdef LIN
// Avoid the mess of linking directly against libcurl at compile time and not finding this version at runtime by
// dynamically loading it at runtime.
#include <dlfcn.h>
#include <array>

// Typedefs matching libcurl function signatures.
// Note: curl_easy_setopt and curl_easy_getinfo use variadic arguments.
using CurlGlobalInitFn = CURLcode (*)(long flags);
using CurlGlobalCleanupFn = void (*)();
using CurlEasyInitFn = void* (*)();
using CurlEasyCleanupFn = void (*)(void* handle);
using CurlEasySetoptFn = CURLcode (*)(void* handle, int option, ...);
using CurlEasyGetinfoFn = CURLcode (*)(void* handle, int info, ...);
using CurlEasyPerformFn = CURLcode (*)(void* handle);
using CurlEasyStrErrorFn = const char* (*)(int code);

// Structure holding dynamically resolved libcurl function pointers.
struct LibCurl {
    void* handle;
    CurlGlobalInitFn global_init;
    CurlGlobalCleanupFn global_cleanup;
    CurlEasyInitFn easy_init;
    CurlEasyCleanupFn easy_cleanup;
    CurlEasySetoptFn easy_setopt;
    CurlEasyPerformFn easy_perform;

    CurlEasyGetinfoFn easy_getinfo;
    CurlEasyStrErrorFn easy_strerror;

    bool Load();
};

static LibCurl lib_curl;

// Attempts to dynamically load libcurl from standard candidate library names.
bool LibCurl::Load() {
    constexpr std::array<const char*, 4> kCurlLibraries = {
        "libcurl.so.4",         // Standard SONAME on Ubuntu, Debian, Fedora, Arch, Alpine
        "libcurl.so",           // Generic link (requires libcurl-dev/devel)
        "libcurl-gnutls.so.4",  // Legacy Debian/Ubuntu GnuTLS variant
        "libcurl-nss.so.4"      // Legacy RHEL/Fedora NSS variant
    };

    for (const char* lib_name : kCurlLibraries) {
        handle = dlopen(lib_name, RTLD_LAZY | RTLD_LOCAL);
        if (handle) {
            LogMsg("Successfully loaded libcurl: %s", lib_name);
            break;
        }
    }

    if (!handle) {
        LogMsg("Failed to load libcurl: %s", dlerror());
        return false;
    }

    // Clear existing errors.
    dlerror();

    // Load symbols.
    global_init = reinterpret_cast<CurlGlobalInitFn>(dlsym(handle, "curl_global_init"));
    global_cleanup = reinterpret_cast<CurlGlobalCleanupFn>(dlsym(handle, "curl_global_cleanup"));
    easy_init = reinterpret_cast<CurlEasyInitFn>(dlsym(handle, "curl_easy_init"));
    easy_cleanup = reinterpret_cast<CurlEasyCleanupFn>(dlsym(handle, "curl_easy_cleanup"));
    easy_setopt = reinterpret_cast<CurlEasySetoptFn>(dlsym(handle, "curl_easy_setopt"));
    easy_perform = reinterpret_cast<CurlEasyPerformFn>(dlsym(handle, "curl_easy_perform"));
    easy_getinfo = reinterpret_cast<CurlEasyGetinfoFn>(dlsym(handle, "curl_easy_getinfo"));
    easy_strerror = reinterpret_cast<CurlEasyStrErrorFn>(dlsym(handle, "curl_easy_strerror"));

    const char* dlsym_error = dlerror();
    if (dlsym_error) {
        LogMsg("Failed to resolve libcurl symbols: %s", dlsym_error);
        dlclose(handle);
        return false;
    }

    return true;
}

// Linux
// use redirected libcurl functions
#define CURL_GLOBAL_INIT lib_curl.global_init
#define CURL_GLOBAL_CLEANUP lib_curl.global_cleanup
#define CURL_EASY_INIT lib_curl.easy_init
#define CURL_EASY_CLEANUP lib_curl.easy_cleanup
#define CURL_EASY_SETOPT lib_curl.easy_setopt
#define CURL_EASY_PERFORM lib_curl.easy_perform
#define CURL_EASY_GETINFO lib_curl.easy_getinfo
#define CURL_EASY_STRERROR lib_curl.easy_strerror

#else
// MacOS
// use native libcurl functions
#define CURL_GLOBAL_INIT curl_global_init
#define CURL_GLOBAL_CLEANUP curl_global_cleanup
#define CURL_EASY_INIT curl_easy_init
#define CURL_EASY_CLEANUP curl_easy_cleanup
#define CURL_EASY_SETOPT curl_easy_setopt
#define CURL_EASY_PERFORM curl_easy_perform
#define CURL_EASY_GETINFO curl_easy_getinfo
#define CURL_EASY_STRERROR curl_easy_strerror
#endif

bool HttpGetInitialize() {
#ifdef LIN
    if (!lib_curl.Load()) {
        LogMsg("Failed to load libcurl.");
        return false;
    }
#endif
    CURLcode result = CURL_GLOBAL_INIT(CURL_GLOBAL_ALL);
    LogMsg("HttpGetInitialize result: %d", (result == CURLE_OK));
    return (result == CURLE_OK);
}

void HttpGetFinalize() {
    CURL_GLOBAL_CLEANUP();
}

static size_t
write_cb(const void *ptr, size_t size, size_t nmemb, void *userdata)
{
    auto len = size * nmemb;
    std::string *data = static_cast<std::string *>(userdata);
    data->append((const char *)ptr, len);
    return len;
}

bool
HttpGet(const std::string& url, std::string& data, int timeout) {
    CURL *curl = CURL_EASY_INIT();
    if(curl == NULL)
        return 0;

    CURL_EASY_SETOPT(curl, CURLOPT_URL, url.c_str());
    CURL_EASY_SETOPT(curl, CURLOPT_TIMEOUT, timeout);
    CURL_EASY_SETOPT(curl, CURLOPT_WRITEFUNCTION, write_cb);
    CURL_EASY_SETOPT(curl, CURLOPT_WRITEDATA, (void *)&data);

    CURL_EASY_SETOPT(curl, CURLOPT_HTTPGET, 1L);
    CURL_EASY_SETOPT(curl, CURLOPT_FOLLOWLOCATION, 1L);
    CURLcode res = CURL_EASY_PERFORM(curl);

    // Check for errors
    if(res != CURLE_OK) {
        LogMsg("curl_easy_perform() failed: %s\n", CURL_EASY_STRERROR(res));
        CURL_EASY_CLEANUP(curl);
        return false;
    }

    curl_off_t dl_size;
    res = CURL_EASY_GETINFO(curl, CURLINFO_SIZE_DOWNLOAD_T , &dl_size);
    if(res == CURLE_OK)
        LogMsg("Downloaded %d bytes", (int)dl_size);

    CURL_EASY_CLEANUP(curl);
    return true;
}
#endif
