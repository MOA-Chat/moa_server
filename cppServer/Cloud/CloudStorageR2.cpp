#include "pch.h"
#include "CloudStorageR2.h"

#include <sstream>
#include <iomanip>
#include <ctime>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>


CloudStorageR2::CloudStorageR2(
    const string& accountId,
    const string& bucketName,
    const string& accessKeyId,
    const string& secretAccessKey,
    const string& publicUrlPrefix)
    : _accountId(accountId), _bucketName(bucketName),
      _accessKeyId(accessKeyId), _secretAccessKey(secretAccessKey),
      _publicUrlPrefix(publicUrlPrefix)
{
    _host = _accountId + ".r2.cloudflarestorage.com";
    _httpClient = make_unique<httplib::Client>("https://" + _host);
    _httpClient->set_connection_timeout(10, 0);
    _httpClient->set_read_timeout(30, 0);
}


bool CloudStorageR2::Initialize()
{
    LOG_INFO("[CloudStorageR2] Init: accountId={}, bucket={}", _accountId, _bucketName);

    if (_accountId.empty() || _bucketName.empty() ||
        _accessKeyId.empty() || _secretAccessKey.empty())
    {
        HandleErr("Initialize", "Missing required config (account_id / bucket_name / keys)");
        return false;
    }

    if (_publicUrlPrefix.empty()) {
        LOG_WARN("[CloudStorageR2] public_url not set — download URLs will use pre-signed GET");
    }
    else {
        LOG_INFO("[CloudStorageR2] Public download URL: {}", _publicUrlPrefix);
    }

    LOG_INFO("[CloudStorageR2] Initialized (AWS Sig V4 / S3-compatible)");
    return true;
}


string CloudStorageR2::GenerateUploadUrl(
    const string& /*fileId*/, const string& path,
    const string& mimeType, int64 expiresInSeconds)
{
    try {
        string cleanPath = path;
        if (!cleanPath.empty() && cleanPath.front() == '/') cleanPath.erase(0, 1);
        return BuildPresignedUrl(cleanPath, mimeType, expiresInSeconds);
    }
    catch (const exception& e) {
        HandleErr("GenerateUploadUrl", e.what());
        return "";
    }
}


string CloudStorageR2::GenerateDownloadUrl(const string& path, int64 /*expiresInSeconds*/)
{
    string cleanPath = path;
    if (!cleanPath.empty() && cleanPath.front() == '/') cleanPath.erase(0, 1);

    // 공개 버킷이면 서명 없이 직접 URL 반환 (다운로드 무료)
    if (!_publicUrlPrefix.empty()) {
        return _publicUrlPrefix + "/" + cleanPath;
    }

    // 공개 URL 없는 경우 pre-signed GET URL 생성 (드물게 사용)
    try {
        return BuildPresignedUrl(cleanPath, "", 31536000);
    }
    catch (const exception& e) {
        HandleErr("GenerateDownloadUrl", e.what());
        return "";
    }
}


bool CloudStorageR2::UploadFile(const string& path, const vector<uint8>& data, const string& mimeType)
{
    try {
        string cleanPath = path;
        if (!cleanPath.empty() && cleanPath.front() == '/') cleanPath.erase(0, 1);

        string bodyStr(data.begin(), data.end());
        string bodySha256   = Sha256Hex(bodyStr);
        string objectPath   = "/" + _bucketName + "/" + cleanPath;
        string contentType  = mimeType.empty() ? "application/octet-stream" : mimeType;

        auto headers = BuildAuthHeaders("PUT", objectPath, bodySha256, contentType);

        auto res = _httpClient->Put(
            objectPath.c_str(), headers,
            reinterpret_cast<const char*>(data.data()), data.size(),
            contentType.c_str());

        if (!res) { HandleErr("UploadFile", "HTTP request failed"); return false; }

        if (res->status == 200 || res->status == 201) {
            LOG_INFO("[CloudStorageR2] File uploaded: {}", path);
            return true;
        }
        HandleErr("UploadFile", "HTTP " + to_string(res->status));
        return false;
    }
    catch (const exception& e) {
        HandleErr("UploadFile", e.what());
        return false;
    }
}


bool CloudStorageR2::RemoveFile(const string& path)
{
    try {
        string cleanPath = path;
        if (!cleanPath.empty() && cleanPath.front() == '/') cleanPath.erase(0, 1);
        string objectPath = "/" + _bucketName + "/" + cleanPath;

        auto headers = BuildAuthHeaders("DELETE", objectPath, EMPTY_SHA256);
        auto res = _httpClient->Delete(objectPath.c_str(), headers);

        if (!res) { HandleErr("RemoveFile", "HTTP request failed"); return false; }

        if (res->status == 204 || res->status == 200) {
            LOG_INFO("[CloudStorageR2] File deleted: {}", path);
            return true;
        }
        if (res->status == 404) {
            LOG_WARN("[CloudStorageR2] File not found (already deleted?): {}", path);
            return false;
        }
        HandleErr("RemoveFile", "HTTP " + to_string(res->status));
        return false;
    }
    catch (const exception& e) {
        HandleErr("RemoveFile", e.what());
        return false;
    }
}


bool CloudStorageR2::FileExists(const string& path)
{
    try {
        string cleanPath = path;
        if (!cleanPath.empty() && cleanPath.front() == '/') cleanPath.erase(0, 1);
        string objectPath = "/" + _bucketName + "/" + cleanPath;

        auto headers = BuildAuthHeaders("HEAD", objectPath, EMPTY_SHA256);
        auto res = _httpClient->Head(objectPath.c_str(), headers);

        if (!res) { HandleErr("FileExists", "HTTP request failed"); return false; }
        return res->status == 200;
    }
    catch (const exception& e) {
        HandleErr("FileExists", e.what());
        return false;
    }
}


/* =====================================================================
                         AWS Signature V4 Helpers
   ===================================================================== */

string CloudStorageR2::GetTimestamp(string& outDateStr) const
{
    time_t now = static_cast<time_t>(Nowts() / 1000);
    struct tm* gmt = gmtime(&now);
    char ts[17], ds[9];
    strftime(ts, sizeof(ts), "%Y%m%dT%H%M%SZ", gmt);
    strftime(ds, sizeof(ds), "%Y%m%d", gmt);
    outDateStr = ds;
    return ts;
}

vector<uint8> CloudStorageR2::HmacSha256Bytes(const vector<uint8>& key, const string& data) const
{
    unsigned char result[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(),
         key.data(), (int)key.size(),
         (const unsigned char*)data.c_str(), data.length(),
         result, &len);
    return vector<uint8>(result, result + len);
}

string CloudStorageR2::HmacSha256Hex(const vector<uint8>& key, const string& data) const
{
    return HexEncode(HmacSha256Bytes(key, data));
}

string CloudStorageR2::Sha256Hex(const string& data) const
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)data.c_str(), data.length(), hash);
    return HexEncode(vector<uint8>(hash, hash + SHA256_DIGEST_LENGTH));
}

string CloudStorageR2::HexEncode(const vector<uint8>& data) const
{
    ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (uint8 b : data) hex << std::setw(2) << (int)b;
    return hex.str();
}

string CloudStorageR2::UrlEncode(const string& str, bool encodeSlash) const
{
    ostringstream out;
    out.fill('0');
    out << std::hex << std::uppercase;
    for (unsigned char c : str) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << c;
        }
        else if (c == '/' && !encodeSlash) {
            out << c;
        }
        else {
            out << '%' << std::setw(2) << (int)c;
        }
    }
    return out.str();
}

vector<uint8> CloudStorageR2::DeriveSigningKey(const string& dateStr) const
{
    string seed = "AWS4" + _secretAccessKey;
    auto kDate    = HmacSha256Bytes(vector<uint8>(seed.begin(), seed.end()), dateStr);
    auto kRegion  = HmacSha256Bytes(kDate,   REGION);
    auto kService = HmacSha256Bytes(kRegion, SERVICE);
    return HmacSha256Bytes(kService, "aws4_request");
}


string CloudStorageR2::BuildPresignedUrl(
    const string& cleanPath, const string& mimeType, int64 expiresInSeconds) const
{
    string dateStr, timestamp = GetTimestamp(dateStr);

    string credentialScope = dateStr + "/" + REGION + "/" + SERVICE + "/aws4_request";
    string credential      = _accessKeyId + "/" + credentialScope;

    bool signContentType = !mimeType.empty();
    string signedHeaders  = signContentType ? "content-type;host" : "host";

    // Query parameters (알파벳 순 필수)
    string q_Algo  = "X-Amz-Algorithm=AWS4-HMAC-SHA256";
    string q_Cred  = "X-Amz-Credential=" + UrlEncode(credential);
    string q_Date  = "X-Amz-Date=" + timestamp;
    string q_Exp   = "X-Amz-Expires=" + to_string(expiresInSeconds);
    string q_Heads = "X-Amz-SignedHeaders=" + UrlEncode(signedHeaders);
    string queryStr = q_Algo + "&" + q_Cred + "&" + q_Date + "&" + q_Exp + "&" + q_Heads;

    // Canonical URI: /<bucket>/<key>  (슬래시는 인코딩하지 않음)
    string canonicalUri = "/" + _bucketName + "/" + UrlEncode(cleanPath, false);

    // Canonical headers
    string canonicalHeaders;
    if (signContentType) canonicalHeaders += "content-type:" + mimeType + "\n";
    canonicalHeaders += "host:" + _host + "\n";

    // Canonical request
    string canonicalRequest =
        "PUT\n"
        + canonicalUri  + "\n"
        + queryStr      + "\n"
        + canonicalHeaders + "\n"
        + signedHeaders + "\n"
        + "UNSIGNED-PAYLOAD";

    // String to sign
    string stringToSign =
        "AWS4-HMAC-SHA256\n"
        + timestamp        + "\n"
        + credentialScope  + "\n"
        + Sha256Hex(canonicalRequest);

    string signature = HmacSha256Hex(DeriveSigningKey(dateStr), stringToSign);

    string url = "https://" + _host + canonicalUri
               + "?" + queryStr
               + "&X-Amz-Signature=" + signature;

    LOG_INFO("[CloudStorageR2] Upload URL generated for: {}", cleanPath);
    return url;
}


httplib::Headers CloudStorageR2::BuildAuthHeaders(
    const string& method, const string& objectPath,
    const string& bodySha256, const string& contentType) const
{
    string dateStr, timestamp = GetTimestamp(dateStr);

    string credentialScope = dateStr + "/" + REGION + "/" + SERVICE + "/aws4_request";

    bool hasContentType = !contentType.empty();

    // SignedHeaders 알파벳 순
    string signedHeaders = "host;x-amz-content-sha256;x-amz-date";
    if (hasContentType) signedHeaders = "content-type;" + signedHeaders;

    // Canonical headers (알파벳 순)
    string canonicalHeaders;
    if (hasContentType) canonicalHeaders += "content-type:" + contentType + "\n";
    canonicalHeaders += "host:" + _host + "\n";
    canonicalHeaders += "x-amz-content-sha256:" + bodySha256 + "\n";
    canonicalHeaders += "x-amz-date:" + timestamp + "\n";

    string canonicalRequest =
        method         + "\n"
        + objectPath   + "\n"
        + "\n"                    // 빈 query string (직접 API 호출)
        + canonicalHeaders + "\n"
        + signedHeaders    + "\n"
        + bodySha256;

    string stringToSign =
        "AWS4-HMAC-SHA256\n"
        + timestamp       + "\n"
        + credentialScope + "\n"
        + Sha256Hex(canonicalRequest);

    string signature = HmacSha256Hex(DeriveSigningKey(dateStr), stringToSign);

    string authorization =
        "AWS4-HMAC-SHA256 Credential=" + _accessKeyId + "/" + credentialScope
        + ", SignedHeaders=" + signedHeaders
        + ", Signature=" + signature;

    httplib::Headers headers = {
        {"Host",                   _host},
        {"x-amz-date",             timestamp},
        {"x-amz-content-sha256",   bodySha256},
        {"Authorization",          authorization},
    };
    if (hasContentType) headers.emplace("Content-Type", contentType);

    return headers;
}
