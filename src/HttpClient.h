#pragma once
#include <string>

struct HttpResponse {
    int statusCode;
    std::string body;
};

HttpResponse HttpPost(const std::wstring& host, int port, const std::wstring& path, const std::string& jsonBody, const std::string& bearerToken);
HttpResponse HttpGet(const std::wstring& host, int port, const std::wstring& path, const std::string& bearerToken);

std::string JsonExtractString(const std::string& json, const std::string& key);
long long JsonExtractNumber(const std::string& json, const std::string& key);
std::string JwtDecodePayload(const std::string& jwt);
