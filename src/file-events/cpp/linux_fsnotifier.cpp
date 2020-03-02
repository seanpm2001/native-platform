#ifdef __linux__

#include <assert.h>
#include <codecvt>
#include <locale>
#include <string>
#include <unistd.h>

#include "linux_fsnotifier.h"

#define EVENT_BUFFER_SIZE 16 * 1024

#define EVENT_MASK (IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_DONT_FOLLOW | IN_EXCL_UNLINK | IN_MODIFY | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO | IN_ONLYDIR)

static int registerWatchPoint(const u16string& path, int fdInotify) {
    string pathNarrow = utf16ToUtf8String(path);
    int fdWatch = inotify_add_watch(fdInotify, pathNarrow.c_str(), EVENT_MASK);
    if (fdWatch == -1) {
        fprintf(stderr, "Couldn't add watch for %s, errno = %d\n", pathNarrow.c_str(), errno);
        throw FileWatcherException("Couldn't add watch");
    }
    return fdWatch;
}

WatchPoint::WatchPoint(const u16string& path, int fdInotify)
    : watchDescriptor(registerWatchPoint(path, fdInotify))
    , fdInotify(fdInotify) {
}

WatchPoint::~WatchPoint() {
    if (inotify_rm_watch(fdInotify, watchDescriptor) != 0) {
        fprintf(stderr, "Couldn't stop watching (inotify = %d, watch descriptor = %d), errno = %d\n", fdInotify, watchDescriptor, errno);
    }
}

static int createInotify() {
    int fdInotify = inotify_init1(IN_CLOEXEC);
    if (fdInotify == -1) {
        fprintf(stderr, "Couldn't register inotify handle, errno = %d\n", errno);
        throw FileWatcherException("register inotify handle");
    }
    return fdInotify;
}

static int createEvent() {
    int fdEvent = eventfd(0, 0);
    if (fdEvent == -1) {
        fprintf(stderr, "Couldn't register inotify handle, errno = %d\n", errno);
        throw FileWatcherException("register inotify handle");
    }
    return fdEvent;
}

Server::Server(JNIEnv* env, jobject watcherCallback)
    : AbstractServer(env, watcherCallback)
    , fdInotify(createInotify())
    , fdProcessCommandsEvent(createEvent()) {
    startThread();
}

void Server::terminate() {
    terminated = true;
}

Server::~Server() {
    // Make copy of watch point paths to avoid race conditions
    list<u16string> paths;
    for (auto& watchPoint : watchPoints) {
        paths.push_back(watchPoint.first);
    }
    for (auto& path : paths) {
        executeOnThread(shared_ptr<Command>(new UnregisterPathCommand(path)));
    }
    executeOnThread(shared_ptr<Command>(new TerminateCommand()));

    if (watcherThread.joinable()) {
        watcherThread.join();
    }

    close(fdInotify);
    close(fdProcessCommandsEvent);
}

void Server::runLoop(JNIEnv* env, function<void(exception_ptr)> notifyStarted) {
    notifyStarted(nullptr);

    char buffer[EVENT_BUFFER_SIZE]
        __attribute__((aligned(__alignof__(struct inotify_event))));

    struct pollfd fds[2];
    fds[0].fd = fdProcessCommandsEvent;
    fds[1].fd = fdInotify;
    fds[0].events = POLLIN;
    fds[1].events = POLLIN;

    while (!terminated) {
        log_fine(env, "Waiting for events (fdInotify = 0x%x)", fdInotify);

        int forever = numeric_limits<int>::max();
        int ret = poll(fds, 2, forever);
        if (ret == -1) {
            log_severe(env, "Couldn't poll: %d, errno = %d", ret, errno);
            throw FileWatcherException("Couldn't poll for events");
        }

        if (IS_SET(fds[0].revents, POLLIN)) {
            uint64_t counter;
            ssize_t bytesRead = read(fdProcessCommandsEvent, &counter, sizeof(counter));
            if (bytesRead == -1) {
                fprintf(stderr, "Couldn't read from event notifier, errno = %d\n", errno);
                throw FileWatcherException("Couldn't read from event notifier");
            }
            // Ignore counter, we only care about the notification itself
            processCommands();
        }

        if (IS_SET(fds[1].revents, POLLIN)) {
            ssize_t bytesRead = read(fdInotify, buffer, EVENT_BUFFER_SIZE);
            if (bytesRead == -1) {
                fprintf(stderr, "Couldn't read from inotify, errno = %d\n", errno);
                throw FileWatcherException("Couldn't read from inotify");
            }
            handleEventsInBuffer(env, buffer, bytesRead);
        }
    }
}

void Server::handleEventsInBuffer(JNIEnv* env, const char* buffer, ssize_t bytesRead) {
    switch (bytesRead) {
        case -1:
            // TODO EINTR is the normal termination, right?
            log_severe(env, "Failed to fetch change notifications, errno = %d", errno);
            terminated = true;
            break;
        case 0:
            terminated = true;
            break;
        default:
            // Handle events
            int index = 0;
            while (index < bytesRead) {
                const struct inotify_event* event = (struct inotify_event*) &buffer[index];
                handleEvent(env, event);
                index += sizeof(struct inotify_event) + event->len;
            }
            break;
    }
}

void Server::processCommandsOnThread() {
    const uint64_t increment = 1;
    write(fdProcessCommandsEvent, &increment, sizeof(increment));
}

void Server::handleEvent(JNIEnv* env, const inotify_event* event) {
    uint32_t mask = event->mask;
    log_fine(env, "Event mask: 0x%x for %s (wd = %d, cookie = 0x%x)", mask, event->name, event->wd, event->cookie);
    if (IS_ANY_SET(mask, IN_UNMOUNT)) {
        return;
    }
    // TODO Do we need error handling here?
    u16string path = watchRoots[event->wd];
    if (IS_SET(mask, IN_IGNORED)) {
        // Finished with watch point
        log_fine(env, "Finished watching", NULL);
        watchPoints.erase(path);
        watchRoots.erase(event->wd);
        return;
    }
    int type;
    const u16string name = utf8ToUtf16String(event->name);
    // TODO How to handle MOVE_SELF?
    if (IS_SET(mask, IN_Q_OVERFLOW)) {
        type = FILE_EVENT_INVALIDATE;
    } else if (IS_ANY_SET(mask, IN_CREATE | IN_MOVED_TO)) {
        type = FILE_EVENT_CREATED;
    } else if (IS_ANY_SET(mask, IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM)) {
        type = FILE_EVENT_REMOVED;
    } else if (IS_SET(mask, IN_MODIFY)) {
        type = FILE_EVENT_MODIFIED;
    } else {
        type = FILE_EVENT_UNKNOWN;
    }
    if (!name.empty()) {
        path.append(u"/");
        path.append(name);
    }
    reportChange(env, type, path);
}

void Server::registerPath(const u16string& path) {
    if (watchPoints.find(path) != watchPoints.end()) {
        throw FileWatcherException("Already watching path");
    }
    auto result = watchPoints.emplace(piecewise_construct,
        forward_as_tuple(path),
        forward_as_tuple(path, fdInotify));
    auto it = result.first;
    watchRoots[it->second.watchDescriptor] = path;
}

void Server::unregisterPath(const u16string& path) {
    auto it = watchPoints.find(path);
    if (it == watchPoints.end()) {
        throw FileWatcherException("Cannot stop watching path that was never watched");
    }
    int wd = it->second.watchDescriptor;
    watchPoints.erase(path);
    watchRoots.erase(wd);
}

JNIEXPORT jobject JNICALL
Java_net_rubygrapefruit_platform_internal_jni_LinuxFileEventFunctions_startWatcher(JNIEnv* env, jclass, jobject javaCallback) {
    return wrapServer(env, [env, javaCallback]() {
        return new Server(env, javaCallback);
    });
}

#endif
