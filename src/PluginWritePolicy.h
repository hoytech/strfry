#pragma once

#include <string.h>
#include <errno.h>
#include <spawn.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <memory>

#include "golpe.h"


struct PluginWritePolicy {
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

        int saveFd(int offset) {
            int fd = fds[offset];
            fds[offset] = -1;
            return fd;
        }
    };

    struct RunningPlugin {
        pid_t pid;
        std::string currPluginPath;
        FILE *r;
        FILE *w;

        RunningPlugin(pid_t pid, int rfd, int wfd, std::string currPluginPath) : pid(pid), currPluginPath(currPluginPath) {
            r = fdopen(rfd, "r");
            w = fdopen(wfd, "w");
            setlinebuf(w);
        }

        ~RunningPlugin() {
            fclose(r);
            fclose(w);
            waitpid(pid, nullptr, 0);
        }
    };

    std::unique_ptr<RunningPlugin> running; 

    bool acceptEvent(std::string_view jsonStr, uint64_t receivedAt, EventSourceType sourceType, std::string_view sourceInfo) {
        if (cfg().relay__plugins__writePolicyPath.size() == 0) return true;

        if (!running) {
            try {
                setupPlugin();
            } catch (std::exception &e) {
                LE << "Couldn't setup PluginWritePolicy: " << e.what();
                return false;
            }
        }

        std::string output;
        output += jsonStr;
        output += "\n";

        ::fwrite(output.data(), output.size(), 1, running->w);

        {
            char buf[4096];
            fgets(buf, sizeof(buf), running->r);
            auto j = tao::json::from_string(buf);
            LI << "QQQ " << j;
        }

        return true;
    }


    void setupPlugin() {
        auto path = cfg().relay__plugins__writePolicyPath;

        Pipe outPipe;
        Pipe inPipe;

        pid_t pid;
        char *argv[] = { nullptr, };

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

        auto ret = posix_spawn(&pid, path.c_str(), &file_actions, nullptr, argv, nullptr);
        if (ret) throw herr("posix_spawn failed when to invoke '", path, "': ", strerror(errno));

        running = make_unique<RunningPlugin>(pid, inPipe.saveFd(0), outPipe.saveFd(1), path);
    }
};
