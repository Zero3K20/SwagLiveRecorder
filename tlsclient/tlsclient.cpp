#include "tlsclient.h"
#include "../chunked_decode.h"
#include <sstream>
#include <algorithm>
#include <winhttp.h>

#define NOMINMAX

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

static bool g_tlsInitialized = false;

// Convert wide string to UTF-8
std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "";
    std::string result(size - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, &result[0], size, nullptr, nullptr);
    return result;
}

// Convert UTF-8 string to wide string
std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (size <= 0) return L"";
    std::wstring result(size - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &result[0], size);
    return result;
}

// Helper: Get HTTP body (skip headers) from raw HTTP response
std::string get_http_body(const std::string& resp) {
    auto pos = resp.find("\r\n\r\n");
    if (pos == std::string::npos) return "";
    return resp.substr(pos + 4);
}

TLSClient::TLSClient() {
    lastError = "";
}

TLSClient::~TLSClient() {
}

void TLSClient::InitializeGlobal() {
    if (!g_tlsInitialized) {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
        g_tlsInitialized = true;
    }
}

bool TLSClient::ParseUrl(const std::string& url, std::string& host, int& port, std::string& path, bool& isHttps) {
    if (url.substr(0, 8) == "https://") {
        isHttps = true;
        port = 443;
        std::string remainder = url.substr(8);
        size_t slashPos = remainder.find('/');
        if (slashPos != std::string::npos) {
            host = remainder.substr(0, slashPos);
            path = remainder.substr(slashPos);
        } else {
            host = remainder;
            path = "/";
        }
    } else if (url.substr(0, 7) == "http://") {
        isHttps = false;
        port = 80;
        std::string remainder = url.substr(7);
        size_t slashPos = remainder.find('/');
        if (slashPos != std::string::npos) {
            host = remainder.substr(0, slashPos);
            path = remainder.substr(slashPos);
        } else {
            host = remainder;
            path = "/";
        }
    } else {
        lastError = "Invalid URL scheme";
        return false;
    }

    // Check for port in host
    size_t colonPos = host.find(':');
    if (colonPos != std::string::npos) {
        port = std::stoi(host.substr(colonPos + 1));
        host = host.substr(0, colonPos);
    }

    return true;
}

bool TLSClient::ParseUrlW(const std::wstring& url, std::string& host, int& port, std::string& path, bool& isHttps) {
    std::string urlStr = WideToUtf8(url);
    return ParseUrl(urlStr, host, port, path, isHttps);
}

bool TLSClient::HttpGet(const std::string& url, std::string& response, const std::string& headers) {
    std::string host, path;
    int port;
    bool isHttps;

    if (!ParseUrl(url, host, port, path, isHttps)) {
        lastError = "Failed to parse URL";
        return false;
    }

    std::wstring wHost = Utf8ToWide(host);
    std::wstring wPath = Utf8ToWide(path);
    std::wstring wHeaders = Utf8ToWide(headers);

    HINTERNET hSession = WinHttpOpen(L"SwagLiveRecorder/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        lastError = "Failed to open WinHTTP session";
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        lastError = "Failed to connect to host";
        return false;
    }

    DWORD dwFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", wPath.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, dwFlags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        lastError = "Failed to create request";
        return false;
    }

    const wchar_t* requestHeaders = wHeaders.empty() ? NULL : wHeaders.c_str();
    BOOL bResult = WinHttpSendRequest(hRequest, requestHeaders, requestHeaders ? (DWORD)-1 : 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(hRequest, NULL);

    if (bResult) {
        DWORD dwSize = 0;
        do {
            DWORD dwDownloaded = 0;
            WinHttpQueryDataAvailable(hRequest, &dwSize);
            if (!dwSize) break;

            size_t prevSize = response.size();
            response.resize(prevSize + dwSize);
            WinHttpReadData(hRequest, &response[prevSize], dwSize, &dwDownloaded);
            if (dwDownloaded < dwSize) {
                response.resize(prevSize + dwDownloaded);
            }
        } while (dwSize > 0);
    } else {
        lastError = "Failed to send request or receive response";
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return bResult != FALSE;
}

bool TLSClient::HttpGetW(const std::wstring& url, std::string& response, const std::wstring& headers) {
    std::string urlStr = WideToUtf8(url);
    std::string headersStr = WideToUtf8(headers);
    return HttpGet(urlStr, response, headersStr);
}

bool TLSClient::HttpPost(const std::string& url, const std::string& postData, std::string& response, const std::string& headers) {
    std::string host, path;
    int port;
    bool isHttps;

    if (!ParseUrl(url, host, port, path, isHttps)) {
        lastError = "Failed to parse URL";
        return false;
    }

    std::wstring wHost = Utf8ToWide(host);
    std::wstring wPath = Utf8ToWide(path);
    std::wstring wHeaders = Utf8ToWide(headers);

    HINTERNET hSession = WinHttpOpen(L"SwagLiveRecorder/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL, NULL, 0);
    if (!hSession) {
        lastError = "Failed to open WinHTTP session";
        return false;
    }

    HINTERNET hConnect = WinHttpConnect(hSession, wHost.c_str(), (INTERNET_PORT)port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        lastError = "Failed to connect to host";
        return false;
    }

    DWORD dwFlags = isHttps ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", wPath.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, dwFlags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        lastError = "Failed to create request";
        return false;
    }

    const wchar_t* requestHeaders = wHeaders.empty() ? NULL : wHeaders.c_str();
    BOOL bSendResult = WinHttpSendRequest(hRequest, requestHeaders, requestHeaders ? (DWORD)-1 : 0,
        (LPVOID)postData.c_str(), (DWORD)postData.length(), (DWORD)postData.length(), 0);

    if (!bSendResult) {
        DWORD error = ::GetLastError();
        lastError = "Failed to send request, error: " + std::to_string(error);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    BOOL bReceiveResult = WinHttpReceiveResponse(hRequest, NULL);
    if (!bReceiveResult) {
        DWORD error = ::GetLastError();
        lastError = "Failed to receive response, error: " + std::to_string(error);
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD dwSize = 0;
    do {
        DWORD dwDownloaded = 0;
        WinHttpQueryDataAvailable(hRequest, &dwSize);
        if (!dwSize) break;

        size_t prevSize = response.size();
        response.resize(prevSize + dwSize);
        WinHttpReadData(hRequest, &response[prevSize], dwSize, &dwDownloaded);
        if (dwDownloaded < dwSize) {
            response.resize(prevSize + dwDownloaded);
        }
    } while (dwSize > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return true;
}

bool TLSClient::HttpPostW(const std::wstring& url, const std::string& postData, std::string& response, const std::wstring& headers) {
    std::string urlStr = WideToUtf8(url);
    std::string headersStr = WideToUtf8(headers);
    return HttpPost(urlStr, postData, response, headersStr);
}

namespace TLSClientHTTP {
    void Initialize() {
        TLSClient::InitializeGlobal();
    }

    std::string HttpGet(const std::wstring& host, const std::wstring& path, const std::wstring& headers) {
        std::wstring url = L"https://" + host + path;
        TLSClient client;
        std::string response;

        if (client.HttpGetW(url, response, headers)) {
            return get_http_body(response);
        }

        return "";
    }

    std::string HttpPost(const std::wstring& host, const std::wstring& path, const std::string& postData, const std::wstring& headers) {
        std::wstring url = L"https://" + host + path;
        TLSClient client;
        std::string response;

        if (client.HttpPostW(url, postData, response, headers)) {
            return get_http_body(response);
        }

        return "";
    }

    bool HttpGetText(const std::wstring& url, std::string& out) {
        TLSClient client;
        std::string response;

        if (client.HttpGetW(url, response)) {
            out = get_http_body(response);
            return !out.empty();
        }

        return false;
    }
}
