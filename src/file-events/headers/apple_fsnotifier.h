#pragma once

#if defined(__APPLE__)

#include "generic_fsnotifier.h"
#include "net_rubygrapefruit_platform_internal_jni_OsxFileEventFunctions.h"
#include <CoreServices/CoreServices.h>
#include <list>

using namespace std;

class Server;

static void handleEventsCallback(
    ConstFSEventStreamRef streamRef,
    void* clientCallBackInfo,
    size_t numEvents,
    void* eventPaths,
    const FSEventStreamEventFlags eventFlags[],
    const FSEventStreamEventId*);

class WatchPoint {
public:
    WatchPoint(Server* server, CFRunLoopRef runLoop, const u16string& path, long latencyInMillis);
    ~WatchPoint();

private:
    FSEventStreamRef watcherStream;
};

class Server : AbstractServer {
public:
    Server(JNIEnv* env, jobject watcherCallback);
    ~Server();

    void startWatching(const u16string& path, long latencyInMillis);
    void handleEvents(
        size_t numEvents,
        char** eventPaths,
        const FSEventStreamEventFlags eventFlags[]);

protected:
    void runLoop(JNIEnv* env, function<void(exception_ptr)> notifyStarted) override;

private:
    void handleEvent(JNIEnv* env, char* path, FSEventStreamEventFlags flags);

    list<WatchPoint> watchPoints;
    CFRunLoopRef threadLoop;
    CFRunLoopTimerRef keepAlive;
};

#endif