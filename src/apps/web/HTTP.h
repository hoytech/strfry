#pragma once

#include <zlib.h>
#include <openssl/sha.h>

#include <mutex>



struct HTTPRequest : NonCopyable {
    uint64_t connId;
    uWS::HttpResponse *res;

    std::string ipAddr;
    std::string url;
    uWS::HttpMethod method = uWS::HttpMethod::METHOD_INVALID;
    bool acceptGzip = false;

    std::string body;

    HTTPRequest(uint64_t connId, uWS::HttpResponse *res, uWS::HttpRequest req) : connId(connId), res(res) {
        res->hasHead = true; // We'll be sending our own headers

        ipAddr = res->httpSocket->getAddressBytes();
        method = req.getMethod();
        url = req.getUrl().toString();
        acceptGzip = req.getHeader("accept-encoding").toStringView().find("gzip") != std::string::npos;
    }
};


struct HTTPResponse : NonCopyable {
    std::string_view code = "200 OK";
    std::string_view contentType = "text/html; charset=utf-8";
    std::string extraHeaders;
    std::string body;

    std::string eTag() {
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<unsigned char*>(body.data()), body.size(), hash);
        return to_hex(std::string_view(reinterpret_cast<char*>(hash), SHA256_DIGEST_LENGTH/2));
    }

    std::string encode(bool doCompress) {
        std::string compressed;
        bool didCompress = false;

        if (doCompress) {
            compressed.resize(body.size());

            z_stream zs;
            zs.zalloc = Z_NULL;
            zs.zfree = Z_NULL;
            zs.opaque = Z_NULL;
            zs.avail_in = body.size();
            zs.next_in = (Bytef*)body.data();
            zs.avail_out = compressed.size();
            zs.next_out = (Bytef*)compressed.data();

            deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY);
            auto ret1 = deflate(&zs, Z_FINISH);
            auto ret2 = deflateEnd(&zs);

            if (ret1 == Z_STREAM_END && ret2 == Z_OK) {
                compressed.resize(zs.total_out);
                didCompress = true;
            }
        }

        auto bodySize = didCompress ? compressed.size() : body.size();

        std::string output;
        output.reserve(bodySize + 1024);

        output += "HTTP/1.1 ";
        output += code;
        output += "\r\nContent-Length: ";
        output += std::to_string(bodySize);
        output += "\r\nContent-Type: ";
        output += contentType;
        output += "\r\n";
        if (didCompress) output += "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n";
        output += extraHeaders;
        output += "Connection: Keep-Alive\r\n\r\n";
        output += didCompress ? compressed : body;

        return output;
    }
};
