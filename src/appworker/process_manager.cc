// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "process_manager.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <boost/bind.hpp>
#include <boost/lexical_cast.hpp>
#include <glog/logging.h>

#include "utils.h"
#include "protocol/galaxy.pb.h"

DECLARE_int32(process_manager_loop_wait_interval);

namespace baidu {
namespace galaxy {

ProcessManager::ProcessManager() :
        mutex_(),
        background_pool_(1) {
    background_pool_.DelayTask(
        FLAGS_process_manager_loop_wait_interval,
        boost::bind(&ProcessManager::LoopWaitProcesses, this)
    );
}

ProcessManager::~ProcessManager() {
}

int ProcessManager::CreateProcess(const ProcessEnv& env,
                                  const ProcessContext* context) {
    // 1.if old process exit, then kill and erase
    {
        MutexLock scope_lock(&mutex_);
        std::map<std::string, Process*>::iterator it =\
            processes_.find(context->process_id);
        if (it != processes_.end()) {
            int32_t pid = it->second->pid;
            killpg(pid, SIGKILL);
            processes_.erase(it);
        }
    }
    LOG(INFO) << "create process, command: " << context->cmd;

    // 2. prepare std fds for child
    int stdin_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
    if (!process::PrepareStdFds(context->work_dir, context->process_id,
                                &stdout_fd, &stderr_fd)) {
        if (stdout_fd != -1) {
            ::close(stdout_fd);
        }
        if (stderr_fd != -1) {
            ::close(stderr_fd);
        }
        LOG(WARNING) << context->process_id << " prepare std file failed";
        return -1;
    }

    // 3. Fork
    pid_t child_pid = ::fork();
    if (child_pid == -1) {
        LOG(WARNING)\
            << context->process_id << " fork failed"\
            << ", errno: " << errno << ", err: "  << strerror(errno);
        if (stdout_fd != -1) {
            ::close(stdout_fd);
        }
        if (stderr_fd != -1) {
            ::close(stderr_fd);
        }
        return -1;
    } else if (child_pid == 0) {
        // 1.setpgid  & chdir
        pid_t my_pid = ::getpid();
        process::PrepareChildProcessEnvStep1(my_pid,
                                             context->work_dir.c_str());
        // 2.dup std fds
        std::vector<int> fd_vector;
        process::PrepareChildProcessEnvStep2(stdin_fd, stdout_fd,
                                             stderr_fd, fd_vector);
        // 3.attach cgroup
        std::vector<std::string>::const_iterator c_it = env.cgroup_paths.begin();
        for (; c_it != env.cgroup_paths.end(); ++c_it) {
            std::string path = *c_it + "/tasks";
            fprintf(stdout, "attach pid to cgroup, pid: %d, cgroup: %s\n",
                    my_pid, path.c_str());
            std::string content = boost::lexical_cast<std::string>(my_pid);
            bool ok = file::Write(path, content);
            if (!ok) {
               fprintf(stdout, "atttach pid to cgroup fail, err: %d, %s\n",
                       errno, strerror(errno));
               fflush(stdout);
               assert(0);
            }
        }

        // 4.prepare argv
        char* argv[] = {
            const_cast<char*>("sh"),
            const_cast<char*>("-c"),
            const_cast<char*>(context->cmd.c_str()),
            NULL};
        // 5.prepare envs
        char* envs[env.envs.size() + 1];
        envs[env.envs.size()]= NULL;
        for (unsigned i = 0; i < env.envs.size(); i++) {
            envs[i] = const_cast<char*>(env.envs[i].c_str());
        }
        // 6.different with deploy and main process
        const DownloadProcessContext* download_context =\
             dynamic_cast<const DownloadProcessContext*>(context);
        if (NULL != download_context) {
            // do deploy, if file no change then exit
            fprintf(stdout, "execve deploy process\n");
            std::string md5;
            if (file::IsExists(download_context->dst_path)
                && file::GetFileMd5(download_context->dst_path, md5)
                && md5 == download_context->version) {
                // data not change
                fprintf(stdout, "data not change, md5: %s", md5.c_str());
                fflush(stdout);
                exit(0);
            }
        }
        // 7.do exec
        ::execve("/bin/sh", argv, envs);
        fprintf(stdout, "execve %s err[%d: %s]\n",
                context->cmd.c_str(), errno, strerror(errno));
        fflush(stdout);
        assert(0);
    }

    if (stdout_fd != -1) {
        ::close(stdout_fd);
    }
    if (stderr_fd != -1) {
        ::close(stderr_fd);
    }
    Process* process = new Process();
    process->process_id = context->process_id;
    process->pid = child_pid;
    process->status = proto::kProcessRunning;
    {
        MutexLock scope_lock(&mutex_);
        processes_.insert(std::make_pair(context->process_id, process));
    }

    return 0;
}

int ProcessManager::KillProcess(const std::string& process_id) {
    MutexLock scope_lock(&mutex_);
    std::map<std::string, Process*>::iterator it = processes_.find(process_id);
    if (it == processes_.end()) {
        LOG(INFO) << "process: " << process_id << " not exist";
        return 0;
    }
    ::killpg(it->second->pid, SIGKILL);

    return 0;
}

int ProcessManager::QueryProcess(const std::string& process_id,
                                 Process& process) {
    MutexLock scope_lock(&mutex_);
    std::map<std::string, Process*>::iterator it = processes_.find(process_id);
    if (it == processes_.end()) {
        LOG(WARNING) << "process: " << process_id << " not exist";
        return -1;
    }
    process.process_id = process_id;
    process.pid = it->second->pid;
    process.status = it->second->status;
    process.exit_code = it->second->exit_code;

    return 0;
}

int ProcessManager::ClearProcesses() {
    MutexLock scope_lock(&mutex_);
    std::map<std::string, Process*>::iterator it = processes_.begin();
    for (; it != processes_.end(); ++it) {
        processes_.erase(it);
    }

    return 0;
}

void ProcessManager::LoopWaitProcesses() {
    MutexLock scope_lock(&mutex_);
    int status = 0;
    pid_t pid = ::waitpid(-1, &status, WNOHANG);
    if (pid > 0) {
        std::map<std::string, Process*>::iterator it = processes_.begin();
        for (; it != processes_.end(); ++it) {
            if (it->second->pid != pid) {
                continue;
            }
            it->second->status = proto::kProcessFinished;
            if (WIFEXITED(status)) {
                it->second->exit_code = WEXITSTATUS(status);
                if (0 != it->second->exit_code) {
                    it->second->status = proto::kProcessFailed;
                }
            } else {
               if (WIFSIGNALED(status)) {
                    it->second->exit_code = 128 + WTERMSIG(status);
                    if (WCOREDUMP(status)) {
                        it->second->status = proto::kProcessCoreDump;
                    } else {
                        it->second->status = proto::kProcessKilled;
                    }
                }
            }
            LOG(WARNING)\
                <<  "process: " << it->second->process_id\
                << ", pid: " << pid\
                << ", exit code: " << it->second->exit_code;
            break;
        }
    }
    background_pool_.DelayTask(
        FLAGS_process_manager_loop_wait_interval,
        boost::bind(&ProcessManager::LoopWaitProcesses, this)
    );

    return;
}

} // ending namespace galaxy
} // ending namespace baidu