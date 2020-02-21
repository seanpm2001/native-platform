#if defined(_WIN32) || defined(__APPLE__)

#include "generic_fsnotifier.h"

class JNIThread {
public:
    JNIThread(JavaVM* jvm, const char* name, bool daemon) {
        this->jvm = jvm;

        JNIEnv* env;
        JavaVMAttachArgs args = {
            JNI_VERSION_1_6,            // version
            const_cast<char*>(name),    // thread name
            NULL                        // thread group
        };
        jint ret = daemon
            ? jvm->AttachCurrentThreadAsDaemon((void**) &env, (void*) &args)
            : jvm->AttachCurrentThread((void**) &env, (void*) &args);
        if (ret != JNI_OK) {
            fprintf(stderr, "Failed to attach JNI to current thread: %d\n", ret);
            throw new FileWatcherException("Failed to attach JNI to current thread");
        }
    }
    ~JNIThread() {
        jint ret = jvm->DetachCurrentThread();
        if (ret != JNI_OK) {
            fprintf(stderr, "Failed to detach JNI from current thread: %d\n", ret);
        }
    }

private:
    JavaVM* jvm;
};

AbstractServer::AbstractServer(JNIEnv* env, jobject watcherCallback) {
    JavaVM* jvm;
    int jvmStatus = env->GetJavaVM(&jvm);
    if (jvmStatus < 0) {
        throw FileWatcherException("Could not store jvm instance");
    }
    this->jvm = jvm;

    jclass callbackClass = env->GetObjectClass(watcherCallback);
    this->watcherCallbackMethod = env->GetMethodID(callbackClass, "pathChanged", "(ILjava/lang/String;)V");

    jobject globalWatcherCallback = env->NewGlobalRef(watcherCallback);
    if (globalWatcherCallback == NULL) {
        throw FileWatcherException("Could not get global ref for watcher callback");
    }
    this->watcherCallback = globalWatcherCallback;
}

AbstractServer::~AbstractServer() {
    JNIEnv* env = getThreadEnv();
    if (env != NULL) {
        env->DeleteGlobalRef(watcherCallback);
    }
}

void AbstractServer::startThread() {
    unique_lock<mutex> lock(watcherThreadMutex);
    this->watcherThread = thread(&AbstractServer::run, this);
    this->watcherThreadStarted.wait(lock);
    if (initException) {
        if (watcherThread.joinable()) {
            watcherThread.join();
        }
        rethrow_exception(initException);
    }
}

void AbstractServer::run() {
    JNIThread jniThread(jvm, "File watcher server", true);
    JNIEnv* env = getThreadEnv();

    log_fine(env, "Starting thread", NULL);

    runLoop(env, [this](exception_ptr initException) {
        unique_lock<mutex> lock(watcherThreadMutex);
        this->initException = initException;
        watcherThreadStarted.notify_all();
        log_fine(getThreadEnv(), "Started thread", NULL);
    });

    log_fine(env, "Stopping thread", NULL);
}

JNIEnv* AbstractServer::getThreadEnv() {
    JNIEnv* env;
    jint ret = jvm->GetEnv((void**) &(env), JNI_VERSION_1_6);
    if (ret != JNI_OK) {
        fprintf(stderr, "Failed to get JNI env for current thread: %d\n", ret);
        throw FileWatcherException("Failed to get JNI env for current thread");
    }
    return env;
}

void AbstractServer::reportChange(JNIEnv* env, int type, const u16string& path) {
    jstring javaPath = env->NewString((jchar*) path.c_str(), (jsize) path.length());
    env->CallVoidMethod(watcherCallback, watcherCallbackMethod, type, javaPath);
    env->DeleteLocalRef(javaPath);
}

#endif