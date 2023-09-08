// Code are copied from https://gist.github.com/vvb2060/a3d40084cd9273b65a15f8a351b4eb0e#file-am_proc_start-cpp

#include <unistd.h>
#include <string>
#include <cinttypes>
#include <android/log.h>
#include <sys/system_properties.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include "crawl_procfs.hpp"
#include "cus.hpp"
#include <libgen.h>
#include <sys/mount.h>
#include <vector>
#include <sys/sendfile.h>
#include <fcntl.h>
#include <sys/prctl.h>

#include "logging.h"

using namespace std;

int myself;

#define API_VERSION "4"

#define CMD_UID_GRANTED_ROOT 12
#define CMD_UID_SHOULD_UMOUNT 13

char *MODPATH = nullptr;
char *MODNAME = nullptr;
vector<string> module_list;

int prop_mirror = -1;
int prop_status = -1;

extern "C" {

struct logger_entry {
    uint16_t len;      /* length of the payload */
    uint16_t hdr_size; /* sizeof(struct logger_entry) */
    int32_t pid;       /* generating process's pid */
    uint32_t tid;      /* generating process's tid */
    uint32_t sec;      /* seconds since Epoch */
    uint32_t nsec;     /* nanoseconds */
    uint32_t lid;      /* log id of the payload, bottom 4 bits currently */
    uint32_t uid;      /* generating process's uid */
};

#define LOGGER_ENTRY_MAX_LEN (5 * 1024)
struct log_msg {
    union [[gnu::aligned(4)]] {
        unsigned char buf[LOGGER_ENTRY_MAX_LEN + 1];
        struct logger_entry entry;
    };
};

[[gnu::weak]] struct logger_list *android_logger_list_alloc(int mode, unsigned int tail, pid_t pid);
[[gnu::weak]] void android_logger_list_free(struct logger_list *list);
[[gnu::weak]] int android_logger_list_read(struct logger_list *list, struct log_msg *log_msg);
[[gnu::weak]] struct logger *android_logger_open(struct logger_list *list, log_id_t id);

typedef struct [[gnu::packed]] {
    int32_t tag;  // Little Endian Order
} android_event_header_t;

typedef struct [[gnu::packed]] {
    int8_t type;   // EVENT_TYPE_INT
    int32_t data;  // Little Endian Order
} android_event_int_t;

typedef struct [[gnu::packed]] {
    int8_t type;     // EVENT_TYPE_STRING;
    int32_t length;  // Little Endian Order
    char data[];
} android_event_string_t;

typedef struct [[gnu::packed]] {
    int8_t type;  // EVENT_TYPE_LIST
    int8_t element_count;
} android_event_list_t;

// 30014 am_proc_start (User|1|5),(PID|1|5),(UID|1|5),(Process Name|3),(Type|3),(Component|3)
typedef struct [[gnu::packed]] {
    android_event_header_t tag;
    android_event_list_t list;
    android_event_int_t user;
    android_event_int_t pid;
    android_event_int_t uid;
    android_event_string_t process_name;
//  android_event_string_t type;
//  android_event_string_t component;
} android_event_am_proc_start;

}

bool is_unmount_target(int uid) {
    int32_t result = -1;
    bool umount = false;
    prctl(0xdeadbeef, CMD_UID_SHOULD_UMOUNT, uid % 100000, &umount, &result);
    return umount;
}


bool is_granted_root_target(int uid) {
    int32_t result = -1;
    bool granted = false;
    prctl(0xdeadbeef, CMD_UID_GRANTED_ROOT, uid % 100000, &granted, &result);
    return granted;
}

int run_script(const char *arg1, const char *arg2, int pid, int uid, const char *arg5, int user){
    int ksu_version = -1;
    prctl(0xdeadbeef, 2, &ksu_version, 0, 0);

    string BB = "/data/adb/modules/magisk_proc_monitor/busybox";
    int p_fork = fork();
    int status = 0;
    char pid_str[10];
    char uid_str[10];
    char user_str[10];
    char ksu_version_str[30];
    snprintf(pid_str, 10, "%d", pid);
    snprintf(uid_str, 10, "%d", uid);
    snprintf(user_str, 10, "%d", user);
    snprintf(ksu_version_str, 30, "%d", ksu_version);
    if (p_fork == 0){
    	// GENERAL
        setenv("API_VERSION", API_VERSION, 1);
        setenv("ASH_STANDALONE", "1", 1);
        // ksu API
        setenv("KSU_VERSION", ksu_version_str, 1);
        setenv("KSU_ON_UNMOUNT", (is_unmount_target(uid))? "true" : "false", 1);
        setenv("KSU_ON_GRANTED", (is_granted_root_target(uid))? "true" : "false", 1);
        //
        execl(BB.data(), "sh", arg1, arg2, pid_str, uid_str, arg5, user_str, (char*)0);
        _exit(1);
    } else if (p_fork > 0){
        waitpid(p_fork, &status, 0);
        return status;
    } else {
        return -1;
    }
}

void run_scripts(int pid, int uid, const char *process, int user) {
    do {
        kill(pid, SIGSTOP);
        struct stat ppid_st, pid_st, init_st;
        int i=0;
        vector<string> module_run;
        vector<string> module_run_st2;

        // run script before enter the mount namespace of target app process
        for (auto i = 0; i < module_list.size(); i++){
            string script = "/data/adb/modules/"s + module_list[i] + "/dynmount.sh"s;
            if (access(script.data(), F_OK) != 0) continue;
            LOGI("run %s#prepareEnterMntNs [%s] pid=[%d]", module_list[i].data(), process, pid);
            int ret = run_script(script.data(), "prepareEnterMntNs", pid, uid, process, user);
                LOGI("run %s#prepareEnterMntNs [%s] pid=[%d] exited with code %d", module_list[i].data(), process, pid, ret/256);
            if (ret == 0) module_run.emplace_back(module_list[i]);
        }
        kill(pid, SIGCONT);

        // if there is no script we want to run in app mount namespace
        if (module_run.size() < 1) {
            LOGI("no module to run EnterMntNs [%s] pid=[%d]", process, pid);
            return;
        }
        do {
            if (i>=30000) {
                LOGW("timeout for wait EnterMntNs [%s] pid=[%d], the mount namespace is not unshared!", process, pid);
                return;
            }
            if (read_ns(pid,&pid_st) == -1 ||
                read_ns(parse_ppid(pid),&ppid_st) == -1)
                return;
            usleep(100);
            i++;
        } while ((pid_st.st_ino == ppid_st.st_ino &&
                pid_st.st_dev == ppid_st.st_dev) ||
                 (pid_st.st_ino == init_st.st_ino &&
                pid_st.st_dev == init_st.st_dev));
        
        // run script after enter the mount namespace of target app process
        // magisk module can modify mount namespce by doing mount/unmount
        kill(pid, SIGSTOP);
        if (!switch_mnt_ns(pid)){
            for (auto i = 0; i < module_run.size(); i++){
                string script = "/data/adb/modules/"s + module_run[i] + "/dynmount.sh"s;
                // run script
                LOGI("run %s#EnterMntNs [%s] pid=[%d]", module_run[i].data(), process, pid);
                int ret = run_script(script.data(), "EnterMntNs", pid, uid, process, user);
                LOGI("run %s#EnterMntNs [%s] pid=[%d] exited with code %d", module_run[i].data(), process, pid, ret/256);
                if (ret == 0) module_run_st2.emplace_back(module_run[i]);
            }
            if (module_run_st2.size() < 1) {
                LOGI("no module to run OnSetUID [%s] pid=[%d]", process, pid);
                goto unblock_process;
            }
            kill(pid, SIGCONT);
            string path = "/proc/"s + to_string(pid);
            int count = 0;
            do {
                int ret = stat(path.data(), &pid_st);
                count++;
                if (count >= 30000) {
                    LOGW("timeout for wait OnSetUID [%s] pid=[%d], uid is not changed", process, pid);
                    goto unblock_process;
                }
                if (ret == -1)
                    continue;
                usleep(100);
            } while (pid_st.st_uid != uid);
            kill(pid, SIGSTOP);
            
            for (auto i = 0; i < module_run_st2.size(); i++){
                string script = "/data/adb/modules/"s + module_run_st2[i] + "/dynmount.sh"s;
                // run script
                LOGI("run %s#OnSetUID [%s] pid=[%d]", module_run_st2[i].data(), process, pid);
                int ret = run_script(script.data(), "OnSetUID", pid, uid, process, user);
                LOGI("run %s#OnSetUID [%s] pid=[%d] exited with code %d", module_run[i].data(), process, pid, ret/256);
            }
        }
        unblock_process:
        kill(pid, SIGCONT);
        return;
    } while (false);
}


void run_daemon(int pid, int uid, const char *process, int user){
    if (fork_dont_care()==0){
        run_scripts(pid,uid,process,user);
        _exit(0);
    }
}



void ProcessBuffer(struct logger_entry *buf) {
    auto *eventData = reinterpret_cast<const unsigned char *>(buf) + buf->hdr_size;
    auto *event_header = reinterpret_cast<const android_event_header_t *>(eventData);
    if (event_header->tag != 30014) return;
    auto *am_proc_start = reinterpret_cast<const android_event_am_proc_start *>(eventData);
    {
        ___write("\U0001F60A Process monitor is working fine");
        char process_name[4098];
        // process name
        snprintf(process_name, 4098, "%.*s", am_proc_start->process_name.length, am_proc_start->process_name.data);
        run_daemon(am_proc_start->pid.data, am_proc_start->uid.data, process_name, am_proc_start->user.data);
    }
}

[[noreturn]] void Run() {
    while (true) {
        bool first;
        bool work = false;
        __system_property_set("persist.log.tag", "");

        unique_ptr<logger_list, decltype(&android_logger_list_free)> logger_list{
            android_logger_list_alloc(0, 1, 0), &android_logger_list_free};
        auto *logger = android_logger_open(logger_list.get(), LOG_ID_EVENTS);
        if (logger != nullptr) [[likely]] {
            first = true;
        } else {
            continue;
        }

        struct log_msg msg{};
        while (true) {
            if (android_logger_list_read(logger_list.get(), &msg) <= 0) [[unlikely]] {
                break;
            }
            if (first) [[unlikely]] {
                first = false;
                continue;
            }
            if (!work) {
                ___write("\U0001F60A Process monitor is working fine but am_proc_start is not detected");
                work = true;
            }
            ProcessBuffer(&msg.entry);
        }
        ___write("\U0001F635 Logcat is not working or broken!");
        work = false;
        sleep(1);
    }
}

void kill_other(struct stat me){
    crawl_procfs([=](int pid) -> bool {
        struct stat st;
        char path[128];
        sprintf(path, "/proc/%d/exe", pid);
        if (stat(path,&st)!=0)
            return true;
        if (pid == myself)
            return true;
        if (st.st_dev == me.st_dev && st.st_ino == me.st_ino) {
            fprintf(stderr, "Killed: %d\n", pid);
            kill(pid, SIGKILL);
        }
        return true;
    });
}

void prepare_modules(){
    string MODULEDIR = "/data/adb/modules";
    DIR *dirfp = opendir(MODULEDIR.data());
    if (dirfp != nullptr){
        dirent *dp;
        while ((dp = readdir(dirfp))) {
            if (dp->d_name == "."sv || dp->d_name == ".."sv) continue;
            char buf[strlen(MODULEDIR.data()) + 1 + strlen(dp->d_name) + 20];
            snprintf(buf, sizeof(buf), "%s/%s/", MODULEDIR.data(), dp->d_name);
            char *z = buf + strlen(MODULEDIR.data()) + 1 + strlen(dp->d_name) + 1;
            strcpy(z, "disable");
            if (access(buf, F_OK) == 0) continue;
            strcpy(z, "remove");
            if (access(buf, F_OK) == 0) continue;
            LOGI("Magisk module: %s", dp->d_name);
            module_list.emplace_back(string(dp->d_name));
        }
        closedir(dirfp);
    }
}
            

int main(int argc, char *argv[]) {
    if (getuid()!=0) return 1;
    struct stat me;
    myself = self_pid();
    
    if (stat(argv[0],&me)!=0)
        return 1;

    if (argc > 1 && argv[1] == "--stop"sv) {
        kill_other(me);
        return 0;
    }
    if (argc > 1 && argv[1] == "--start"sv) {
        kill_other(me);
        prop_mirror = open(PROPFILE, O_RDONLY);
        prop_status = open(WPROPFILE , O_RDWR | O_CREAT, 0644);
        struct stat stat_buf;
        if (stat(PROPFILE, &stat_buf) == 0 && sendfile(prop_status, prop_mirror, 0, stat_buf.st_size) > 0) {
            mount(WPROPFILE, PROPFILE, nullptr, MS_BIND, nullptr);
            mount(nullptr, PROPFILE, nullptr, MS_PRIVATE, nullptr);
        }
        set_nice_name(SECRETNAME);
        if (fork_dont_care()==0){
            fprintf(stderr, "New daemon: %d\n", self_pid());
            if (switch_mnt_ns(1))
                _exit(0);
            MODPATH = dirname(argv[0]);
            MODNAME = basename(MODPATH);
            signal(SIGTERM, SIG_IGN);
            prepare_modules();
            Run();
            _exit(0);
        }
        return 0;
    }
    Run();
}
