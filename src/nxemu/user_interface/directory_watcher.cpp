#include "directory_watcher.h"
#include <chrono>

namespace
{
constexpr std::chrono::milliseconds QuietPeriod(1500);
}

#if defined(_WIN32)
#include <windows.h>
#include <thread>
#include <yuzu_common/string_util.h>

struct DirectoryWatcher::Impl
{
    struct WatchedDir
    {
        HANDLE dir = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped = {};
        BOOL subtree = FALSE;
        DWORD buffer[4096];
    };

    Impl(const std::vector<std::string> & dirs, const std::vector<std::string> & recursiveDirs, ChangeCallback callback) :
        m_callback(std::move(callback)),
        m_stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
        for (const std::string & dir : dirs)
        {
            Add(dir, FALSE);
        }
        for (const std::string & dir : recursiveDirs)
        {
            Add(dir, TRUE);
        }
        m_thread = std::thread(&Impl::Run, this);
    }

    ~Impl()
    {
        SetEvent(m_stopEvent);
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        for (std::unique_ptr<WatchedDir> & watched : m_dirs)
        {
            Close(*watched);
        }
        CloseHandle(m_stopEvent);
    }

    void Add(const std::string & path, BOOL subtree)
    {
        if (path.empty() || m_dirs.size() >= MAXIMUM_WAIT_OBJECTS - 1)
        {
            return;
        }
        std::unique_ptr<WatchedDir> watched = std::make_unique<WatchedDir>();
        watched->subtree = subtree;
        watched->dir = CreateFileW(Common::UTF8ToUTF16W(path).c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (watched->dir == INVALID_HANDLE_VALUE)
        {
            return;
        }
        watched->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (watched->overlapped.hEvent == nullptr || !Read(*watched))
        {
            Close(*watched);
            return;
        }
        m_dirs.push_back(std::move(watched));
    }

    static bool Read(WatchedDir & watched)
    {
        const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE;
        ResetEvent(watched.overlapped.hEvent);
        return ReadDirectoryChangesW(watched.dir, watched.buffer, sizeof(watched.buffer), watched.subtree, filter, nullptr, &watched.overlapped, nullptr) != 0;
    }

    static void Close(WatchedDir & watched)
    {
        if (watched.dir != INVALID_HANDLE_VALUE)
        {
            DWORD bytes = 0;
            if (CancelIoEx(watched.dir, &watched.overlapped) || GetLastError() != ERROR_NOT_FOUND)
            {
                GetOverlappedResult(watched.dir, &watched.overlapped, &bytes, TRUE);
            }
            CloseHandle(watched.dir);
            watched.dir = INVALID_HANDLE_VALUE;
        }
        if (watched.overlapped.hEvent != nullptr)
        {
            CloseHandle(watched.overlapped.hEvent);
            watched.overlapped.hEvent = nullptr;
        }
    }

    void Run()
    {
        bool pending = false;
        for (;;)
        {
            std::vector<HANDLE> handles;
            handles.push_back(m_stopEvent);
            for (const std::unique_ptr<WatchedDir> & watched : m_dirs)
            {
                handles.push_back(watched->overlapped.hEvent);
            }

            const DWORD timeout = pending ? (DWORD)QuietPeriod.count() : INFINITE;
            const DWORD result = WaitForMultipleObjects((DWORD)handles.size(), handles.data(), FALSE, timeout);
            if (result == WAIT_TIMEOUT)
            {
                pending = false;
                m_callback();
                continue;
            }
            if (result == WAIT_OBJECT_0 || result < WAIT_OBJECT_0 || result >= WAIT_OBJECT_0 + handles.size())
            {
                break;
            }

            WatchedDir & watched = *m_dirs[result - WAIT_OBJECT_0 - 1];
            DWORD bytes = 0;
            GetOverlappedResult(watched.dir, &watched.overlapped, &bytes, FALSE);
            pending = true;
            if (!Read(watched))
            {
                Close(watched);
                m_dirs.erase(m_dirs.begin() + (result - WAIT_OBJECT_0 - 1));
            }
        }
    }

    ChangeCallback m_callback;
    HANDLE m_stopEvent;
    std::vector<std::unique_ptr<WatchedDir>> m_dirs;
    std::thread m_thread;
};

#elif defined(__APPLE__)
#include <CoreServices/CoreServices.h>
#include <atomic>
#include <dispatch/dispatch.h>

struct DirectoryWatcher::Impl
{
    struct State
    {
        ChangeCallback callback;
        uint64_t generation = 0;
        bool stopped = false;
    };

    struct Pending
    {
        std::shared_ptr<State> state;
        uint64_t generation;
    };

    Impl(const std::vector<std::string> & dirs, const std::vector<std::string> & recursiveDirs, ChangeCallback callback) :
        m_state(std::make_shared<State>()),
        m_queue(dispatch_queue_create("nxemu.directory_watcher", DISPATCH_QUEUE_SERIAL)),
        m_stream(nullptr)
    {
        m_state->callback = std::move(callback);

        CFMutableArrayRef paths = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
        for (const std::vector<std::string> * list : {&dirs, &recursiveDirs})
        {
            for (const std::string & dir : *list)
            {
                if (dir.empty())
                {
                    continue;
                }
                CFStringRef path = CFStringCreateWithCString(nullptr, dir.c_str(), kCFStringEncodingUTF8);
                if (path != nullptr)
                {
                    CFArrayAppendValue(paths, path);
                    CFRelease(path);
                }
            }
        }

        if (CFArrayGetCount(paths) > 0)
        {
            FSEventStreamContext context = {};
            context.info = this;
            m_stream = FSEventStreamCreate(nullptr, &Impl::StreamCallback, &context, paths, kFSEventStreamEventIdSinceNow, 0.5, kFSEventStreamCreateFlagNone);
            if (m_stream != nullptr)
            {
                FSEventStreamSetDispatchQueue(m_stream, m_queue);
                FSEventStreamStart(m_stream);
            }
        }
        CFRelease(paths);
    }

    ~Impl()
    {
        if (m_stream != nullptr)
        {
            FSEventStreamStop(m_stream);
            FSEventStreamInvalidate(m_stream);
            FSEventStreamRelease(m_stream);
        }
        dispatch_sync_f(m_queue, m_state.get(), [](void * context) { ((State *)context)->stopped = true; });
        dispatch_release(m_queue);
    }

    static void StreamCallback(ConstFSEventStreamRef, void * info, size_t, void *, const FSEventStreamEventFlags *, const FSEventStreamEventId *)
    {
        Impl * impl = (Impl *)info;
        Pending * pending = new Pending{impl->m_state, ++impl->m_state->generation};
        const dispatch_time_t when = dispatch_time(DISPATCH_TIME_NOW, (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(QuietPeriod).count());
        dispatch_after_f(when, impl->m_queue, pending, [](void * context) {
            std::unique_ptr<Pending> pending((Pending *)context);
            if (!pending->state->stopped && pending->generation == pending->state->generation)
            {
                pending->state->callback();
            }
        });
    }

    std::shared_ptr<State> m_state;
    dispatch_queue_t m_queue;
    FSEventStreamRef m_stream;
};

#elif defined(__linux__)
#include <cerrno>
#include <filesystem>
#include <map>
#include <poll.h>
#include <sys/inotify.h>
#include <thread>
#include <unistd.h>
#include <fcntl.h>

struct DirectoryWatcher::Impl
{
    Impl(const std::vector<std::string> & dirs, const std::vector<std::string> & recursiveDirs, ChangeCallback callback) :
        m_callback(std::move(callback)),
        m_inotify(inotify_init1(IN_NONBLOCK | IN_CLOEXEC)),
        m_stopPipe{-1, -1}
    {
        if (m_inotify < 0 || pipe2(m_stopPipe, O_NONBLOCK | O_CLOEXEC) != 0)
        {
            return;
        }
        for (const std::string & dir : dirs)
        {
            Add(dir, false);
        }
        for (const std::string & dir : recursiveDirs)
        {
            Add(dir, true);
        }
        m_thread = std::thread(&Impl::Run, this);
    }

    ~Impl()
    {
        if (m_thread.joinable())
        {
            const char stop = 1;
            [[maybe_unused]] const ssize_t written = write(m_stopPipe[1], &stop, 1);
            m_thread.join();
        }
        for (int fd : {m_inotify, m_stopPipe[0], m_stopPipe[1]})
        {
            if (fd >= 0)
            {
                close(fd);
            }
        }
    }

    void Add(const std::string & path, bool recursive)
    {
        if (path.empty())
        {
            return;
        }
        const uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF;
        const int wd = inotify_add_watch(m_inotify, path.c_str(), mask);
        if (wd < 0)
        {
            return;
        }
        m_watches[wd] = {path, recursive};
        if (!recursive)
        {
            return;
        }
        std::error_code ec;
        for (std::filesystem::directory_iterator itr(path, ec), end; !ec && itr != end; itr.increment(ec))
        {
            if (itr->is_directory(ec) && !itr->is_symlink(ec))
            {
                Add(itr->path().string(), true);
            }
        }
    }

    void Run()
    {
        bool pending = false;
        alignas(inotify_event) char buffer[16 * 1024];
        for (;;)
        {
            pollfd fds[2] = {{m_inotify, POLLIN, 0}, {m_stopPipe[0], POLLIN, 0}};
            const int timeout = pending ? (int)QuietPeriod.count() : -1;
            const int result = poll(fds, 2, timeout);
            if (result < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                break;
            }
            if (result == 0)
            {
                pending = false;
                m_callback();
                continue;
            }
            if ((fds[1].revents & POLLIN) != 0)
            {
                break;
            }
            for (;;)
            {
                const ssize_t length = read(m_inotify, buffer, sizeof(buffer));
                if (length <= 0)
                {
                    break;
                }
                for (ssize_t offset = 0; offset < length;)
                {
                    const inotify_event * event = (const inotify_event *)(buffer + offset);
                    offset += sizeof(inotify_event) + event->len;
                    pending = true;

                    const bool newDir = (event->mask & IN_ISDIR) != 0 && (event->mask & (IN_CREATE | IN_MOVED_TO)) != 0;
                    const std::map<int, Watch>::const_iterator watch = m_watches.find(event->wd);
                    if (newDir && watch != m_watches.end() && watch->second.recursive && event->len > 0)
                    {
                        Add(watch->second.path + "/" + event->name, true);
                    }
                    if ((event->mask & IN_IGNORED) != 0)
                    {
                        m_watches.erase(event->wd);
                    }
                }
            }
        }
    }

    struct Watch
    {
        std::string path;
        bool recursive;
    };

    ChangeCallback m_callback;
    int m_inotify;
    int m_stopPipe[2];
    std::map<int, Watch> m_watches;
    std::thread m_thread;
};

#else

struct DirectoryWatcher::Impl
{
    Impl(const std::vector<std::string> &, const std::vector<std::string> &, ChangeCallback)
    {
    }
};

#endif

DirectoryWatcher::DirectoryWatcher()
{
}

DirectoryWatcher::~DirectoryWatcher()
{
    Stop();
}

void DirectoryWatcher::Start(const std::vector<std::string> & dirs, const std::vector<std::string> & recursiveDirs, ChangeCallback callback)
{
    Stop();
    m_impl = std::make_unique<Impl>(dirs, recursiveDirs, std::move(callback));
}

void DirectoryWatcher::Stop()
{
    m_impl.reset();
}
