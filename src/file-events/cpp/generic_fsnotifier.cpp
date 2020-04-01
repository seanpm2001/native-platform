#include <codecvt>
#include <locale>
#include <sstream>
#include <string>

#include "generic_fsnotifier.h"

inline string createMessage(const string& message, const u16string& path) {
    stringstream ss;
    ss << message;
    ss << ": ";
    ss << utf16ToUtf8String(path);
    return ss.str();
}

inline string createMessage(const string& message, int errorCode) {
    stringstream ss;
    ss << message;
    ss << ", error = ";
    ss << errorCode;
    return ss.str();
}

inline string createMessage(const string& message, const u16string& path, int errorCode) {
    stringstream ss;
    ss << message;
    ss << ", error = ";
    ss << errorCode;
    ss << ": ";
    ss << utf16ToUtf8String(path);
    return ss.str();
}

FileWatcherException::FileWatcherException(const string& message, const u16string& path, int errorCode)
    : runtime_error(createMessage(message, path, errorCode)) {
}

FileWatcherException::FileWatcherException(const string& message, const u16string& path)
    : runtime_error(createMessage(message, path)) {
}

FileWatcherException::FileWatcherException(const string& message, int errorCode)
    : runtime_error(createMessage(message, errorCode)) {
}

FileWatcherException::FileWatcherException(const string& message)
    : runtime_error(message) {
}

AbstractServer::AbstractServer(JNIEnv* env, jobject watcherCallback)
    : JniSupport(env)
    , watcherCallback(env, watcherCallback) {
    jclass callbackClass = env->GetObjectClass(watcherCallback);
    this->watcherCallbackMethod = env->GetMethodID(callbackClass, "pathChanged", "(ILjava/lang/String;)V");
    this->watcherReportErrorMethod = env->GetMethodID(callbackClass, "reportError", "(Ljava/lang/Throwable;)V");
}

AbstractServer::~AbstractServer() {
}

void AbstractServer::startThread() {
    unique_lock<mutex> lock(watcherThreadMutex);
    this->watcherThread = thread(&AbstractServer::run, this);
    auto status = this->watcherThreadStarted.wait_for(lock, THREAD_TIMEOUT);
    if (status == cv_status::timeout) {
        throw FileWatcherException("Starting thread timed out");
    }
    if (initException) {
        if (watcherThread.joinable()) {
            watcherThread.join();
        }
        rethrow_exception(initException);
    }
}

void AbstractServer::run() {
    JniThreadAttacher jniThread(jvm, "File watcher server", true);
    logToJava(FINE, "Starting thread", NULL);

    try {
        runLoop([this](exception_ptr initException) {
            unique_lock<mutex> lock(watcherThreadMutex);
            this->initException = initException;
            watcherThreadStarted.notify_all();
            logToJava(FINE, "Started thread", NULL);
        });
    } catch (const exception& ex) {
        reportError(getThreadEnv(), ex);
    }

    logToJava(FINE, "Stopping thread", NULL);
}

bool AbstractServer::executeOnThread(shared_ptr<Command> command) {
    unique_lock<mutex> lock(mtxCommands);
    commands.push_back(command);
    processCommandsOnThread();
    auto status = command->executed.wait_for(lock, THREAD_TIMEOUT);
    if (status == cv_status::timeout) {
        throw FileWatcherException("Command execution timed out");
    }
    if (command->failure) {
        rethrow_exception(command->failure);
    } else {
        return command->success;
    }
}

void AbstractServer::processCommands() {
    unique_lock<mutex> lock(mtxCommands);
    for (auto& command : commands) {
        command->execute(this);
    }
    commands.clear();
}

void AbstractServer::reportChange(JNIEnv* env, int type, const u16string& path) {
    jstring javaPath = env->NewString((jchar*) path.c_str(), (jsize) path.length());
    env->CallVoidMethod(watcherCallback.get(), watcherCallbackMethod, type, javaPath);
    env->DeleteLocalRef(javaPath);
    JniSupport::rethrowJavaException(env);
}

void AbstractServer::reportError(JNIEnv* env, const exception& exception) {
    u16string message = utf8ToUtf16String(exception.what());
    jstring javaMessage = env->NewString((jchar*) message.c_str(), (jsize) message.length());
    jmethodID constructor = env->GetMethodID(nativeContants->nativeExceptionClass.get(), "<init>", "(Ljava/lang/String;)V");
    jobject javaException = env->NewObject(nativeContants->nativeExceptionClass.get(), constructor, javaMessage);
    env->CallVoidMethod(watcherCallback.get(), watcherReportErrorMethod, javaException);
    env->DeleteLocalRef(javaMessage);
    env->DeleteLocalRef(javaException);
    JniSupport::rethrowJavaException(env);
}

AbstractServer* getServer(JNIEnv* env, jobject javaServer) {
    AbstractServer* server = (AbstractServer*) env->GetDirectBufferAddress(javaServer);
    if (server == NULL) {
        throw FileWatcherException("Closed already");
    }
    return server;
}

jobject rethrowAsJavaException(JNIEnv* env, const exception& e) {
    logToJava(SEVERE, "Caught exception: %s", e.what());
    jint ret = env->ThrowNew(nativeContants->nativeExceptionClass.get(), e.what());
    if (ret != 0) {
        cerr << "JNI ThrowNew returned %d when rethrowing native exception: " << ret << endl;
    }
    return NULL;
}

jobject wrapServer(JNIEnv* env, function<void*()> serverStarter) {
    void* server;
    try {
        server = serverStarter();
    } catch (const exception& e) {
        return rethrowAsJavaException(env, e);
    }

    jmethodID constructor = env->GetMethodID(nativeContants->nativeFileWatcherClass.get(), "<init>", "(Ljava/lang/Object;)V");
    return env->NewObject(nativeContants->nativeFileWatcherClass.get(), constructor, env->NewDirectByteBuffer(server, sizeof(server)));
}

void AbstractServer::registerPaths(const vector<u16string>& paths) {
    for (auto& path : paths) {
        registerPath(path);
    }
}

bool AbstractServer::unregisterPaths(const vector<u16string>& paths) {
    bool success = true;
    for (auto& path : paths) {
        success &= unregisterPath(path);
    }
    return success;
}

JNIEXPORT void JNICALL
Java_net_rubygrapefruit_platform_internal_jni_AbstractFileEventFunctions_00024NativeFileWatcher_startWatching0(JNIEnv* env, jobject, jobject javaServer, jobjectArray javaPaths) {
    try {
        AbstractServer* server = getServer(env, javaServer);
        vector<u16string> paths;
        javaToUtf16StringArray(env, javaPaths, paths);
        server->executeOnThread(shared_ptr<Command>(new RegisterPathsCommand(paths)));
    } catch (const exception& e) {
        rethrowAsJavaException(env, e);
    }
}

JNIEXPORT jboolean JNICALL
Java_net_rubygrapefruit_platform_internal_jni_AbstractFileEventFunctions_00024NativeFileWatcher_stopWatching0(JNIEnv* env, jobject, jobject javaServer, jobjectArray javaPaths) {
    try {
        AbstractServer* server = getServer(env, javaServer);
        vector<u16string> paths;
        javaToUtf16StringArray(env, javaPaths, paths);
        return server->executeOnThread(shared_ptr<Command>(new UnregisterPathsCommand(paths)));
    } catch (const exception& e) {
        rethrowAsJavaException(env, e);
        return false;
    }
}

JNIEXPORT void JNICALL
Java_net_rubygrapefruit_platform_internal_jni_AbstractFileEventFunctions_00024NativeFileWatcher_close0(JNIEnv* env, jobject, jobject javaServer) {
    try {
        AbstractServer* server = getServer(env, javaServer);
        delete server;
    } catch (const exception& e) {
        rethrowAsJavaException(env, e);
    }
}

JNIEXPORT void JNICALL Java_net_rubygrapefruit_platform_internal_jni_AbstractFileEventFunctions_invalidateLogLevelCache0(JNIEnv*, jobject) {
    logging->invalidateLogLevelCache();
}

NativeConstants::NativeConstants(JavaVM* jvm)
    : JniSupport(jvm)
    , nativeExceptionClass(getThreadEnv(), "net/rubygrapefruit/platform/NativeException")
    , nativeFileWatcherClass(getThreadEnv(), "net/rubygrapefruit/platform/internal/jni/AbstractFileEventFunctions$NativeFileWatcher") {
}
