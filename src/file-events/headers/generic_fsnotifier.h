#pragma once

#include "logging.h"
#include <exception>
#include <functional>
#include <mutex>
#include <thread>

using namespace std;

// Corresponds to values of FileWatcherCallback.Type
#define FILE_EVENT_CREATED 0
#define FILE_EVENT_REMOVED 1
#define FILE_EVENT_MODIFIED 2
#define FILE_EVENT_INVALIDATE 3
#define FILE_EVENT_UNKNOWN 4

#define IS_SET(flags, flag) (((flags) & (flag)) == (flag))
#define IS_ANY_SET(flags, mask) (((flags) & (mask)) != 0)

struct FileWatcherException : public exception {
public:
    FileWatcherException(const char* message) {
        this->message = message;
    }

    const char* what() const throw() {
        return message;
    }

private:
    const char* message;
};

class AbstractServer {
public:
    AbstractServer(JNIEnv* env, jobject watcherCallback);
    virtual ~AbstractServer();

    JNIEnv* getThreadEnv();

protected:
    void reportChange(JNIEnv* env, int type, const u16string& path);

    void startThread();
    virtual void runLoop(JNIEnv* env, function<void(exception_ptr)> notifyStarted) = 0;

    thread watcherThread;

private:
    void run();
    mutex watcherThreadMutex;
    condition_variable watcherThreadStarted;
    exception_ptr initException;

    jobject watcherCallback;
    jmethodID watcherCallbackMethod;

    JavaVM* jvm;
};