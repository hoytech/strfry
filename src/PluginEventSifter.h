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

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
#define st_mtim st_mtimespec
#endif

#include "hoytech/stream.h"

#include "golpe.h"

#include "events.h"

#if defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__) || defined(__DragonFly__)
extern char **environ;
#elif defined(__APPLE__)
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
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
  uint64_t nextRequestSeq = 1;

    PluginEventSifterResult acceptEvent(const std::string &pluginCmd, const tao::json::value &evJson, EventSourceType sourceType, std::string_view sourceInfo, const Bytes32 &authed, std::string &okMsg) {
        if (pluginCmd.size() == 0) {
            running.reset();
            return PluginEventSifterResult::Accept;
        }

    try {
      ensurePluginRunning(pluginCmd, "write policy");

      auto requestId = makeRequestId();

            auto request = tao::json::value({
                { "type", "new" },
                { "event", evJson },
                { "receivedAt", ::time(nullptr) },
                { "sourceType", eventSourceTypeToStr(sourceType) },
                { "sourceInfo", sourceType == EventSourceType::IP4 || sourceType == EventSourceType::IP6 ? renderIP(sourceInfo) : sourceInfo },
            });

            if (!authed.isNull()) request["authed"] = to_hex(authed.sv());

      auto response =
          callPlugin(pluginCmd, "write policy", request, requestId,
                     cfg().relay__writePolicy__timeoutSeconds * 1'000);

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

  PluginEventSifterResult preAuth(const std::string &pluginCmd,
                                  const tao::json::value &authEventJson,
                                  EventSourceType sourceType,
                                  std::string_view sourceInfo, uint64_t connId,
                                  const Bytes32 &authed, std::string &okMsg) {
    if (pluginCmd.size() == 0) {
      running.reset();
      return PluginEventSifterResult::Accept;
    }

    try {
      ensurePluginRunning(pluginCmd, "pre-auth");

      auto requestId = makeRequestId();

      auto request = tao::json::value({
          {"type", "authPre"},
          {"id", requestId},
          {"event", authEventJson},
          {"receivedAt", ::time(nullptr)},
          {"sourceType", eventSourceTypeToStr(sourceType)},
          {"sourceInfo", sourceType == EventSourceType::IP4 ||
                                 sourceType == EventSourceType::IP6
                             ? renderIP(sourceInfo)
                             : sourceInfo},
          {"connId", connId},
      });

      if (!authed.isNull())
        request["authed"] = to_hex(authed.sv());

      auto response =
          callPlugin(pluginCmd, "pre-auth", request, requestId,
                     cfg().relay__auth__pluginTimeoutSeconds * 1'000);

      okMsg = response.optional<std::string>("msg").value_or("");

      auto action = response.at("action").get_string();
      if (action == "accept")
        return PluginEventSifterResult::Accept;
      else if (action == "reject")
        return PluginEventSifterResult::Reject;
      else if (action == "shadowReject")
        return PluginEventSifterResult::Reject;
      else
        throw herr("unknown action: ", action);
    } catch (std::exception &e) {
      LE << "Pre-auth plugin error: " << e.what();
      running.reset();
      okMsg = "error: internal error";
      return PluginEventSifterResult::Reject;
    }
  }

  void postAuth(const std::string &pluginCmd,
                const tao::json::value &authEventJson,
                EventSourceType sourceType, std::string_view sourceInfo,
                uint64_t connId, const Bytes32 &authed) {
    if (pluginCmd.size() == 0) {
      running.reset();
      return;
    }

    try {
      ensurePluginRunning(pluginCmd, "post-auth");

      auto requestId = makeRequestId();

      auto request = tao::json::value({
          {"type", "authPost"},
          {"id", requestId},
          {"event", authEventJson},
          {"receivedAt", ::time(nullptr)},
          {"sourceType", eventSourceTypeToStr(sourceType)},
          {"sourceInfo", sourceType == EventSourceType::IP4 ||
                                 sourceType == EventSourceType::IP6
                             ? renderIP(sourceInfo)
                             : sourceInfo},
          {"connId", connId},
      });

      if (!authed.isNull())
        request["authed"] = to_hex(authed.sv());

      (void)callPlugin(pluginCmd, "post-auth", request, requestId,
                       cfg().relay__auth__pluginTimeoutSeconds * 1'000);
    } catch (std::exception &e) {
      LE << "Post-auth plugin error: " << e.what();
      running.reset();
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
  std::string makeRequestId() {
    return std::to_string(::time(nullptr)) + ":" +
           std::to_string(nextRequestSeq++);
  }

  void ensurePluginRunning(const std::string &pluginCmd,
                           std::string_view pluginName) {
    if (running) {
      if (pluginCmd != running->currPluginCmd) {
        running.reset();
      } else if (pluginCmd.find(' ') == std::string::npos) {
        struct stat statbuf;
        if (stat(pluginCmd.c_str(), &statbuf))
          throw herr("couldn't stat plugin: ", pluginCmd);
        if (statbuf.st_mtim.tv_sec != running->lastModTime.tv_sec ||
            statbuf.st_mtim.tv_nsec != running->lastModTime.tv_nsec) {
          running.reset();
        }
      }
    }

    if (!running) {
      setupPlugin(pluginCmd, pluginName);
    }
  }

  tao::json::value callPlugin(const std::string &pluginCmd,
                              std::string_view pluginName,
                              const tao::json::value &request,
                              std::string_view requestId, uint64_t timeoutMs) {
    std::string output = tao::json::to_string(request);
    output += "\n";

    try {
      running->streamWriter.write(output, timeoutMs);
    } catch (std::exception &e) {
      throw herr("Failed to write to ", pluginName, " plugin: ", e.what(),
                 ". Request was: ", output);
    }

    while (1) {
      std::string line;

      try {
        line = running->streamReader.read(timeoutMs);
      } catch (std::exception &e) {
        throw herr("Failed to read ", pluginName,
                   " plugin response: ", e.what(), ". Request was: ", output);
      }

      tao::json::value response;

      try {
        response = tao::json::from_string(line);
      } catch (std::exception &) {
        LW << "Got unparseable line from " << pluginName << " plugin: " << line;
        continue;
      }

      if (!response.is_object() || !response.at("id").is_string()) {
        throw herr("plugin response missing string id");
      }

      if (response.at("id").get_string() != requestId) {
        throw herr("plugin response id mismatch");
      }

      return response;
    }
  }

  void setupPlugin(const std::string &pluginCmd, std::string_view pluginName) {
    LI << "Setting up " << pluginName << " plugin: " << pluginCmd;

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
