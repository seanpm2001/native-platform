#ifdef _WIN32

#include "win_fsnotifier.h"

using namespace std;

//
// WatchPoint
//

WatchPoint::WatchPoint(Server* server, size_t bufferSize, const u16string& path)
    : path(path)
    , status(NOT_LISTENING) {
    wstring pathW(path.begin(), path.end());
    HANDLE directoryHandle = CreateFileW(
        pathW.c_str(),          // pointer to the file name
        FILE_LIST_DIRECTORY,    // access (read/write) mode
        CREATE_SHARE,           // share mode
        NULL,                   // security descriptor
        OPEN_EXISTING,          // how to create
        CREATE_FLAGS,           // file attributes
        NULL                    // file with attributes to copy
    );
    if (directoryHandle == INVALID_HANDLE_VALUE) {
        throw FileWatcherException("Couldn't add watch", path, GetLastError());
    }
    this->directoryHandle = directoryHandle;

    this->server = server;
    this->buffer.reserve(bufferSize);
    ZeroMemory(&this->overlapped, sizeof(OVERLAPPED));
    this->overlapped.hEvent = this;
    switch (listen()) {
        case SUCCESS:
            break;
        case DELETED:
            throw FileWatcherException("Couldn't start watching because path is not a directory", path);
    }
}

bool WatchPoint::cancel() {
    if (status == LISTENING) {
        logToJava(FINE, "Cancelling %s", utf16ToUtf8String(path).c_str());
        status = CANCELLED;
        bool cancelled = (bool) CancelIoEx(directoryHandle, &overlapped);
        if (!cancelled) {
            DWORD cancelError = GetLastError();
            close();
            if (cancelError == ERROR_NOT_FOUND) {
                // Do nothing, looks like this is a typical scenario
                logToJava(FINE, "Watch point already finished %s", utf16ToUtf8String(path).c_str());
            } else {
                throw FileWatcherException("Couldn't cancel watch point", path, cancelError);
            }
        }
        return cancelled;
    }
    return false;
}

WatchPoint::~WatchPoint() {
    try {
        if (cancel()) {
            SleepEx(0, true);
        }
    } catch (const exception& ex) {
        logToJava(WARNING, "Couldn't cancel watch point %s: %s", utf16ToUtf8String(path).c_str(), ex.what());
    }
}

static void CALLBACK handleEventCallback(DWORD errorCode, DWORD bytesTransferred, LPOVERLAPPED overlapped) {
    WatchPoint* watchPoint = (WatchPoint*) overlapped->hEvent;
    watchPoint->handleEventsInBuffer(errorCode, bytesTransferred);
}

bool WatchPoint::isValidDirectory() {
    wstring pathW(path.begin(), path.end());
    DWORD attrib = GetFileAttributesW(pathW.c_str());

    return (attrib != INVALID_FILE_ATTRIBUTES)
        && ((attrib & FILE_ATTRIBUTE_DIRECTORY) != 0);
}

ListenResult WatchPoint::listen() {
    BOOL success = ReadDirectoryChangesW(
        directoryHandle,              // handle to directory
        &buffer[0],                   // read results buffer
        (DWORD) buffer.capacity(),    // length of buffer
        TRUE,                         // include children
        EVENT_MASK,                   // filter conditions
        NULL,                         // bytes returned
        &overlapped,                  // overlapped buffer
        &handleEventCallback          // completion routine
    );
    if (success) {
        status = LISTENING;
        return SUCCESS;
    } else {
        DWORD listenError = GetLastError();
        close();
        if (listenError == ERROR_ACCESS_DENIED && !isValidDirectory()) {
            return DELETED;
        } else {
            throw FileWatcherException("Couldn't start watching", path, listenError);
        }
    }
}

void WatchPoint::close() {
    if (status != FINISHED) {
        BOOL ret = CloseHandle(directoryHandle);
        if (!ret) {
            logToJava(SEVERE, "Couldn't close handle %p for '%ls': %d", directoryHandle, utf16ToUtf8String(path).c_str(), GetLastError());
        }
        status = FINISHED;
    }
}

void WatchPoint::handleEventsInBuffer(DWORD errorCode, DWORD bytesTransferred) {
    if (errorCode == ERROR_OPERATION_ABORTED) {
        logToJava(FINE, "Finished watching '%s', status = %d", utf16ToUtf8String(path).c_str(), status);
        close();
        return;
    }

    if (status != LISTENING) {
        logToJava(FINE, "Ignoring incoming events for %s as watch-point is not listening (%d bytes, errorCode = %d, status = %d)",
            utf16ToUtf8String(path).c_str(), bytesTransferred, errorCode, status);
        return;
    }
    status = NOT_LISTENING;
    server->handleEvents(this, errorCode, buffer, bytesTransferred);
}

void Server::handleEvents(WatchPoint* watchPoint, DWORD errorCode, const vector<BYTE>& buffer, DWORD bytesTransferred) {
    unique_lock<mutex> lock(mutationMutex);
    JNIEnv* env = getThreadEnv();
    const u16string& path = watchPoint->path;

    try {
        if (errorCode != ERROR_SUCCESS) {
            if (errorCode == ERROR_ACCESS_DENIED && !watchPoint->isValidDirectory()) {
                reportChange(env, REMOVED, path);
            } else {
                throw FileWatcherException("Error received when handling events", path, errorCode);
            }
        }

        if (terminated) {
            logToJava(FINE, "Ignoring incoming events for %s because server is terminating (%d bytes, status = %d)",
                utf16ToUtf8String(path).c_str(), bytesTransferred, watchPoint->status);
            return;
        }

        if (bytesTransferred == 0) {
            // This is what the documentation has to say about a zero-length dataset:
            //
            //     If the number of bytes transferred is zero, the buffer was either too large
            //     for the system to allocate or too small to provide detailed information on
            //     all the changes that occurred in the directory or subtree. In this case,
            //     you should compute the changes by enumerating the directory or subtree.
            //
            // (See https://docs.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-readdirectorychangesw)
            //
            // We'll handle this as a simple overflow and report it as such.
            logToJava(INFO, "Detected overflow for %s", utf16ToUtf8String(path).c_str());
            reportChange(env, OVERFLOWED, path);
        } else {
            int index = 0;
            for (;;) {
                FILE_NOTIFY_INFORMATION* current = (FILE_NOTIFY_INFORMATION*) &buffer[index];
                handleEvent(env, path, current);
                if (current->NextEntryOffset == 0) {
                    break;
                }
                index += current->NextEntryOffset;
            }
        }

        switch (watchPoint->listen()) {
            case SUCCESS:
                break;
            case DELETED:
                logToJava(FINE, "Watched directory removed for %s", utf16ToUtf8String(path).c_str());
                reportChange(env, REMOVED, path);
                break;
        }
    } catch (const exception& ex) {
        reportError(env, ex);
    }
}

bool isAbsoluteLocalPath(const u16string& path) {
    if (path.length() < 3) {
        return false;
    }
    return ((u'a' <= path[0] && path[0] <= u'z') || (u'A' <= path[0] && path[0] <= u'Z'))
        && path[1] == u':'
        && path[2] == u'\\';
}

bool isAbsoluteUncPath(const u16string& path) {
    if (path.length() < 3) {
        return false;
    }
    return path[0] == u'\\' && path[1] == u'\\';
}

bool isLongPath(const u16string& path) {
    return path.length() >= 4 && path.substr(0, 4) == u"\\\\?\\";
}

bool isUncLongPath(const u16string& path) {
    return path.length() >= 8 && path.substr(0, 8) == u"\\\\?\\UNC\\";
}

// TODO How can this be done nicer, wihtout both unnecessary copy and in-place mutation?
void convertToLongPathIfNeeded(u16string& path) {
    // Technically, this should be MAX_PATH (i.e. 260), except some Win32 API related
    // to working with directory paths are actually limited to 240. It is just
    // safer/simpler to cover both cases in one code path.
    if (path.length() <= 240) {
        return;
    }

    // It is already a long path, nothing to do here
    if (isLongPath(path)) {
        return;
    }

    if (isAbsoluteLocalPath(path)) {
        // Format: C:\... -> \\?\C:\...
        path.insert(0, u"\\\\?\\");
    } else if (isAbsoluteUncPath(path)) {
        // In this case, we need to skip the first 2 characters:
        // Format: \\server\share\... -> \\?\UNC\server\share\...
        path.erase(0, 2);
        path.insert(0, u"\\\\?\\UNC\\");
    } else {
        // It is some sort of unknown format, don't mess with it
    }
}

void Server::handleEvent(JNIEnv* env, const u16string& path, FILE_NOTIFY_INFORMATION* info) {
    wstring changedPathW = wstring(info->FileName, 0, info->FileNameLength / sizeof(wchar_t));
    u16string changedPath(changedPathW.begin(), changedPathW.end());
    if (!changedPath.empty()) {
        changedPath.insert(0, 1, u'\\');
    }
    changedPath.insert(0, path);
    // TODO Remove long prefix for path once?
    if (isLongPath(changedPath)) {
        if (isUncLongPath(changedPath)) {
            changedPath.erase(0, 8).insert(0, u"\\\\");
        } else {
            changedPath.erase(0, 4);
        }
    }

    logToJava(FINE, "Change detected: 0x%x '%s'", info->Action, utf16ToUtf8String(changedPath).c_str());

    FileWatchEventType type;
    if (info->Action == FILE_ACTION_ADDED || info->Action == FILE_ACTION_RENAMED_NEW_NAME) {
        type = CREATED;
    } else if (info->Action == FILE_ACTION_REMOVED || info->Action == FILE_ACTION_RENAMED_OLD_NAME) {
        type = REMOVED;
    } else if (info->Action == FILE_ACTION_MODIFIED) {
        type = MODIFIED;
    } else {
        logToJava(WARNING, "Unknown event 0x%x for %s", info->Action, utf16ToUtf8String(changedPath).c_str());
        type = UNKNOWN;
    }

    reportChange(env, type, changedPath);
}

//
// Server
//

Server::Server(JNIEnv* env, size_t bufferSize, jobject watcherCallback)
    : AbstractServer(env, watcherCallback)
    , bufferSize(bufferSize) {
}

void Server::initializeRunLoop() {
    // TODO For some reason GetCurrentThread() returns a thread that doesn't accept APCs
    threadHandle = OpenThread(
        THREAD_ALL_ACCESS,      // dwDesiredAccess
        false,                  // bInheritHandle
        GetCurrentThreadId()    // dwThreadId
    );
    if (threadHandle == NULL) {
        throw FileWatcherException("Couldn't open current thread", GetLastError());
    }
}

void Server::terminateRunLoop() {
    executeOnRunLoop([this]() {
        terminated = true;
        return true;
    });
}

void Server::runLoop() {
    while (!terminated) {
        SleepEx(INFINITE, true);
    }

    // We have received termination, cancel all watchers
    unique_lock<mutex> lock(mutationMutex);
    logToJava(FINE, "Finished with run loop, now cancelling remaining watch points", NULL);
    int pendingWatchPoints = 0;
    for (auto& it : watchPoints) {
        auto& watchPoint = it.second;
        switch (watchPoint.status) {
            case LISTENING:
                try {
                    if (watchPoint.cancel()) {
                        pendingWatchPoints++;
                    }
                } catch (const exception& ex) {
                    logToJava(SEVERE, "%s", ex.what());
                }
                break;
            case CANCELLED:
                pendingWatchPoints++;
                break;
            default:
                break;
        }
    }

    // If there are any pending watchers, wait for them to finish
    if (pendingWatchPoints > 0) {
        logToJava(FINE, "Waiting for %d pending watch points to finish", pendingWatchPoints);
        SleepEx(0, true);
    }

    // Warn about  any unfinished watchpoints
    for (auto& it : watchPoints) {
        auto& watchPoint = it.second;
        switch (watchPoint.status) {
            case NOT_LISTENING:
            case FINISHED:
                break;
            default:
                logToJava(WARNING, "Watch point %s did not finish before termination timeout (status = %d)",
                    utf16ToUtf8String(watchPoint.path).c_str(), watchPoint.status);
                break;
        }
    }

    CloseHandle(threadHandle);
}

struct Command {
    Server* server;
    function<bool()> function;
    condition_variable executed;
    bool result;
    exception_ptr failure;
};

static void CALLBACK executeOnRunLoopCallback(_In_ ULONG_PTR info) {
    Command* command = (Command*) info;
    try {
        command->result = command->function();
    } catch (const exception&) {
        command->failure = current_exception();
    }
    unique_lock<mutex> lock(command->server->executionMutex);
    command->executed.notify_all();
}

bool Server::executeOnRunLoop(function<bool()> function) {
    Command command;
    command.function = function;
    command.server = this;
    unique_lock<mutex> lock(executionMutex);
    DWORD ret = QueueUserAPC(executeOnRunLoopCallback, threadHandle, (ULONG_PTR) &command);
    if (ret == 0) {
        throw FileWatcherException("Received error while queuing APC", GetLastError());
    }
    auto status = command.executed.wait_for(lock, THREAD_TIMEOUT);
    if (status == cv_status::timeout) {
        throw FileWatcherException("Execution timed out");
    } else if (command.failure) {
        rethrow_exception(command.failure);
    } else {
        return command.result;
    }
}

void Server::registerPaths(const vector<u16string>& paths) {
    executeOnRunLoop([this, paths]() {
        AbstractServer::registerPaths(paths);
        return true;
    });
}

bool Server::unregisterPaths(const vector<u16string>& paths) {
    return executeOnRunLoop([this, paths]() {
        return AbstractServer::unregisterPaths(paths);
    });
}

void Server::registerPath(const u16string& path) {
    u16string longPath = path;
    convertToLongPathIfNeeded(longPath);
    auto it = watchPoints.find(longPath);
    if (it != watchPoints.end()) {
        if (it->second.status != FINISHED) {
            throw FileWatcherException("Already watching path", path);
        }
        watchPoints.erase(it);
    }
    watchPoints.emplace(piecewise_construct,
        forward_as_tuple(longPath),
        forward_as_tuple(this, bufferSize, longPath));
}

bool Server::unregisterPath(const u16string& path) {
    u16string longPath = path;
    convertToLongPathIfNeeded(longPath);
    if (watchPoints.erase(longPath) == 0) {
        logToJava(INFO, "Path is not watched: %s", utf16ToUtf8String(path).c_str());
        return false;
    }
    return true;
}

//
// JNI calls
//

JNIEXPORT jobject JNICALL
Java_net_rubygrapefruit_platform_internal_jni_WindowsFileEventFunctions_startWatcher0(JNIEnv* env, jclass target, jint bufferSize, jobject javaCallback) {
    return wrapServer(env, new Server(env, bufferSize, javaCallback));
}

#endif
