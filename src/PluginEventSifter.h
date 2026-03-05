#pragma once

#include <string.h>
#include <errno.h>
#include <spawn.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>

#include <memory>

#include "hoytech/stream.h"

#include "golpe.h"

#include "events.h"

#ifdef __FreeBSD__
extern char **environ;
#endif



enum class PluginEventSifterResult {
    Accept,
    Reject,
    ShadowReject,
};


struct PluginEventSifter {
    struct RunningPlugin {
        pid_t pid;
        hoytech::StreamReader streamReader;
        hoytech::StreamWriter streamWriter;
        std::string currPluginCmd;
        struct timespec lastModTime;

        RunningPlugin(pid_t pid, int rfd, int wfd, std::string currPluginCmd) : pid(pid), streamReader(rfd), streamWriter(wfd), currPluginCmd(currPluginCmd) {
            streamReader.setMaxRecordSize(8192);

            if (currPluginCmd.find(' ') == std::string::npos) {
                struct stat statbuf;
                if (stat(currPluginCmd.c_str(), &statbuf)) throw herr("couldn't stat plugin: ", currPluginCmd);
                lastModTime = statbuf.st_mtim;
            }
        }

        ~RunningPlugin() {
            ::kill(pid, SIGTERM);
            ::waitpid(pid, nullptr, 0);
        }
    };

    std::unique_ptr<RunningPlugin> running; 

    PluginEventSifterResult acceptEvent(const std::string &pluginCmd, const tao::json::value &evJson, EventSourceType sourceType, std::string_view sourceInfo, const Bytes32 &authed, std::string &okMsg) {
        if (pluginCmd.size() == 0) {
            running.reset();
            return PluginEventSifterResult::Accept;
        }

        try {
            if (running) {
                if (pluginCmd != running->currPluginCmd) {
                    running.reset();
                } else if (pluginCmd.find(' ') == std::string::npos) {
                    struct stat statbuf;
                    if (stat(pluginCmd.c_str(), &statbuf)) throw herr("couldn't stat plugin: ", pluginCmd);
                    if (statbuf.st_mtim.tv_sec != running->lastModTime.tv_sec || statbuf.st_mtim.tv_nsec != running->lastModTime.tv_nsec) {
                        running.reset();
                    }
                }
            }

            if (!running) {
                setupPlugin(pluginCmd);
            }

            auto request = tao::json::value({
                { "type", "new" },
                { "event", evJson },
                { "receivedAt", ::time(nullptr) },
                { "sourceType", eventSourceTypeToStr(sourceType) },
                { "sourceInfo", sourceType == EventSourceType::IP4 || sourceType == EventSourceType::IP6 ? renderIP(sourceInfo) : sourceInfo },
            });

            if (!authed.isNull()) request["authed"] = to_hex(authed.sv());

            std::string output = tao::json::to_string(request);
            output += "\n";

            try {
                running->streamWriter.write(output, cfg().relay__writePolicy__timeoutSeconds * 1'000);
            } catch (std::exception &e) {
                throw herr("Failed to write event: ", e.what(), ". Request was: ", output);
            }

            tao::json::value response;

            while (1) {
                std::string line;

                try {
                    line = running->streamReader.read(cfg().relay__writePolicy__timeoutSeconds * 1'000);
                } catch (std::exception &e) {
                    throw herr("Failed to read response: ", e.what(), ". Request was: ", output);
                }

                try {
                    response = tao::json::from_string(line);
                } catch (std::exception &e) {
                    LW << "Got unparseable line from write policy plugin: " << line;
                    continue;
                }

                if (response.at("id").get_string() != request.at("event").at("id").get_string()) throw herr("id mismatch");

                break;
            }

            okMsg = response.optional<std::string>("msg").value_or("");

            auto action = response.at("action").get_string();
            if (action == "accept") return PluginEventSifterResult::Accept;
            else if (action == "reject") return PluginEventSifterResult::Reject;
            else if (action == "shadowReject") return PluginEventSifterResult::ShadowReject;
            else throw herr("unknown action: ", action);
        } catch (std::exception &e) {
            LE << "Plugin error: " << e.what();
            running.reset();
            okMsg = "error: internal error";
            return PluginEventSifterResult::Reject;
        }
    }


    struct Pipe : NonCopyable {
        int fds[2] = { -1, -1 };

        Pipe() {
            if (::pipe(fds)) throw herr("pipe failed: ", strerror(errno));
        }

        Pipe(int fd0, int fd1) {
            fds[0] = fd0;
            fds[1] = fd1;
        }

        ~Pipe() {
            if (fds[0] != -1) ::close(fds[0]);
            if (fds[1] != -1) ::close(fds[1]);
        }

        int extractFd(int offset) {
            int fd = fds[offset];
            fds[offset] = -1;
            return fd;
        }
    };

  private:
    void setupPlugin(const std::string &pluginCmd) {
        LI << "Setting up write policy plugin: " << pluginCmd;

        Pipe outPipe;
        Pipe inPipe;

        pid_t pid;
        const char * const argv[] = { "/bin/sh", "-c", pluginCmd.c_str(), nullptr, };

        posix_spawn_file_actions_t file_actions;

        if (
            posix_spawn_file_actions_init(&file_actions) ||
            posix_spawn_file_actions_adddup2(&file_actions, outPipe.fds[0], 0) ||
            posix_spawn_file_actions_adddup2(&file_actions, inPipe.fds[1], 1) ||
            posix_spawn_file_actions_addclose(&file_actions, outPipe.fds[0]) ||
            posix_spawn_file_actions_addclose(&file_actions, outPipe.fds[1]) ||
            posix_spawn_file_actions_addclose(&file_actions, inPipe.fds[0]) ||
            posix_spawn_file_actions_addclose(&file_actions, inPipe.fds[1])
        ) throw herr("posix_span_file_actions failed: ", strerror(errno));

        auto ret = posix_spawnp(&pid, "sh", &file_actions, nullptr, (char* const*)(&argv[0]), environ);
        if (ret) throw herr("posix_spawn failed to invoke '", pluginCmd, "': ", strerror(errno));

        running = make_unique<RunningPlugin>(pid, inPipe.extractFd(0), outPipe.extractFd(1), pluginCmd);
    }
};
