#include <docopt.h>
#include <tao/json.hpp>
#include <hoytech/protected_queue.h>
#include <hoytech/file_change_monitor.h>

#include "golpe.h"

#ifdef _WIN32
#include <io.h>
inline ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    if (lineptr == NULL || n == NULL || stream == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (*lineptr == NULL || *n == 0) {
        *n = 128;
        *lineptr = (char *)malloc(*n);
        if (*lineptr == NULL) {
            return -1;
        }
    }
    size_t count = 0;
    int c;
    while ((c = fgetc(stream)) != EOF) {
        if (count + 1 >= *n) {
            size_t new_len = *n * 2;
            char *new_ptr = (char *)realloc(*lineptr, new_len);
            if (new_ptr == NULL) {
                return -1;
            }
            *lineptr = new_ptr;
            *n = new_len;
        }
        (*lineptr)[count++] = c;
        if (c == '\n') {
            break;
        }
    }
    if (count == 0 && c == EOF) {
        return -1;
    }
    (*lineptr)[count] = '\0';
    return count;
}
#endif

#include "WSConnection.h"


static const char USAGE[] =
R"(
    Usage:
      upload <url> [--pipeline=<pipeline>]

    Options:
      --pipeline=<pipeline>    Number of concurrent in-flight event transmissions. Default is 50.
)";



void cmd_upload(const std::vector<std::string> &subArgs) {
    std::map<std::string, docopt::value> args = docopt::docopt(USAGE, subArgs, true, "");

    std::string url = args["<url>"].asString();

    uint64_t pipeline = 50;
    if (args["--pipeline"]) pipeline = args["--pipeline"].asLong();


    uint64_t numInFlight = 0;
    uint64_t totalUploaded = 0;
    uint64_t totalAccepted = 0;
    WSConnection ws(url);

    size_t bufLen = cfg().events__maxEventSize + 1024;
    char *buf = (char*)::malloc(bufLen);
    bool eof = false;

    auto getNextLine = [&]() -> std::optional<std::string_view> {
        ssize_t numRead = ::getline(&buf, &bufLen, stdin);
        if (numRead <= 0) {
            eof = true;
            return {};
        }

        if ((uint64_t)numRead > cfg().events__maxEventSize) throw herr("line longer than configured maxEventSize");

        if (numRead > 0) numRead--; // chop off newline

        return std::string_view(buf, (size_t)numRead);
    };

    auto sendWsMessage = [&](std::string_view event){
        std::string msg = std::string("[\"EVENT\",");
        msg += event;
        msg += "]";
        ws.send(msg);

        numInFlight++;
    };

    auto processLines = [&]{
        while (!eof && numInFlight < pipeline) {
            auto event = getNextLine();
            if (event) sendWsMessage(*event);
        }
    };

    auto logStats = [&]{
        LI << "Sent " << totalUploaded << " events, " << totalAccepted << " accepted";
    };


    ws.onConnect = [&]{
        processLines();
    };

    ws.onMessage = [&](auto msg, uWS::OpCode, size_t){
        try {
            auto origJson = tao::json::from_string(msg);

            if (origJson.is_array()) {
                if (origJson.get_array().size() < 2) throw herr("array too short");

                auto &msgType = origJson.get_array().at(0);
                if (msgType == "NOTICE") {
                    LW << "NOTICE message: " << tao::json::to_string(origJson);
                    return;
                } else if (msgType == "OK") {
                    if (origJson.get_array().at(2).get_boolean()) {
                        totalAccepted++;
                    } else {
                        LW << "Event not written: " << origJson;
                    }

                    numInFlight--;
                    totalUploaded++;

                    processLines();

                    if (numInFlight == 0) {
                        logStats();
                        ::exit(0);
                    }

                    if (totalUploaded % 1000 == 0) logStats();
                } else {
                    LW << "Unexpected message: " << msg;
                }
            } else {
                LW << "Unexpected message: " << msg;
            }
        } catch (std::exception &e) {
            LE << "Error receiving nostr message: " << e.what() << " message: " << msg;
            ::exit(1);
        }
    };

    ws.run();
}
