#include <unistd.h>
#include <string.h>
#include <sys/epoll.h>
#include <exception>
#include "curl_worker.h"
#include "utils.h"

const int SOCKET_PAIR_WRITE = 0;
const int SOCKET_PAIR_READ  = 1;

const int MAX_BUFFER_SIZE = 4096;

const bool g_debug = false;

#define RUNTIME_EX(...) \
do { \
    char msg[1024]; \
    snprintf(msg, sizeof(msg), __VA_ARGS__); \
    thro wruntime_error(msg); \
} while (0)

struct CurlInfo
{
    CurlInfo() {
        fd = -1;
        handle = NULL;
    }

    int     fd;
    CURL*   handle;
    CurlCallback    cb;
};

// ######################################################
//                    event notify
// ######################################################
struct RequestEvent
{
    CURL*   handle;
};

// ######################################################
//                    CurlEpollWorker
// ######################################################
int CurlEpollWorker::curl_timer_cb(CURLM* multi, long timeout_ms, void* data)
{
    auto worker = (CurlEpollWorker*)data;
    struct itimerspec its;
    if (timeout_ms > 0) {
        its.it_interval.tv_sec = 0;
        its.it_interval.tv_nsec = 0;
        its.it_value.tv_sec = timeout_ms / 1000;
        its.it_value.tv_nsec = (timeout_ms % 1000) * 1000 * 1000;
    } else if (timeout_ms == 0) {
        its.it_interval.tv_sec = 0;
        its.it_interval.tv_nsec = 0;
        its.it_value.tv_sec = 0;
        its.it_value.tv_nsec = 1;
    } else {
        memset(&its, 0, sizeof(its));
    }
    timerfd_settime(worker->_timer_fd, 0, &its, NULL);
    return 0;
}

int CurlEpollWorker::curl_sock_cb(CURL* curl, curl_socket_t sock, int what, void* cbp, void* sockp)
{
    const char* whatstr[] = {"none", "IN", "OUT", "INOUT", "REMOVE"};
    auto worker = (CurlEpollWorker*)cbp;
    worker->deal_sock_event(curl, sock, what);
    return 0;
}

size_t CurlEpollWorker::curl_read_cb(void* ptr, size_t size, size_t nmemb, void* data)
{
    CurlInfo* info = (CurlInfo*)data;
    info->cb.read_cb(info->handle, ptr, size, nmemb);
    return size * nmemb;
}

CurlEpollWorker::CurlEpollWorker(CurlAsyncEngine* engine, int thread_no, int max_conn_count)
{
    _thread_no = thread_no;

    _exit = false;
    _is_running = false;
    _max_conn_count = max_conn_count;

    _epoll_fd = 0;
    _timer_fd = 0;

    _multi = NULL;

    _buf_size = MAX_BUFFER_SIZE;
    _buf_used_size = 0;
    _buffer = (char*)malloc(_buf_size);

    _engine = engine;
}

CurlEpollWorker::~CurlEpollWorker()
{
    if (_is_running) {
        stop();
    }
}

int CurlEpollWorker::start()
{
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, _event_fds) < 0) {
        return -1;
    }

    _thread = thread([this]() {
        LOG("worker_thread:%u", gettid());
        while (1) {
            if (!thread_run()) {
                break ;
            }
            sleep(1);
        }
        free(_buffer);
        LOG("thread:%ld is exit", pthread_self());
    });
    return 0;
}

void CurlEpollWorker::stop(bool wait)
{
    LOG("thread:%d stop", _thread_no);
    close(_event_fds[SOCKET_PAIR_WRITE]);
    if (wait) {
        _thread.join();
    } else {
        _thread.detach();
    }
}

int CurlEpollWorker::request(CURL* handle, CurlCallback cb)
{
    if (!cb.read_cb || !cb.error_cb) {
        return -1;
    }

    auto curl_info = new CurlInfo;
    curl_info->handle = handle;
    curl_info->cb = cb;

    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, CurlEpollWorker::curl_read_cb);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, curl_info);
    curl_easy_setopt(handle, CURLOPT_PRIVATE, curl_info);

    {
        lock_guard<mutex> lock(_mutex);
        _unrelate_curls[handle] = curl_info;
    }

    RequestEvent req = {handle};
    int ret = write(_event_fds[SOCKET_PAIR_WRITE], &req, sizeof(req));
    if (ret == -1) {
        LOG(
            "ERROR|write fd to worker thread fail, fd:%d, error:%s", 
            _event_fds[SOCKET_PAIR_WRITE],
            strerror(errno)
        );
        {
            lock_guard<mutex> lock(_mutex);
            _unrelate_curls.erase(handle);
        }
        curl_easy_cleanup(handle);
        delete curl_info;
        return -1;
    }
    return 0;
}

int CurlEpollWorker::thread_run()
{
    {
        lock_guard<mutex> lock(_mutex);
        if (init_worker_info() != 0) {
            LOG("ERROR|init_worker_info fail");
            return -1;
        }
        _is_running = true;
    }
    LOG("init succeed");

    struct epoll_event* events = 
        (struct epoll_event*)malloc(sizeof(struct epoll_event) * _max_conn_count);

    while (!_exit) {
        int ret = epoll_wait(_epoll_fd, events, _max_conn_count, 1000);
        if (ret == -1) {
            if (errno == EINTR) {
                continue ;
            } else {
                break ;
            }
        }
        for (int i = 0; i < ret; i++) {
            if (events[i].data.fd == _event_fds[SOCKET_PAIR_READ]) {
                on_request();
            } else if (events[i].data.fd == _timer_fd) {
                on_timer();
            } else {
                on_event(events[i].data.fd, events[i].events);
            }
        }
    }

    {
        lock_guard<mutex> lock(_mutex);
        _is_running = false;
    }
    release_worker_info();
    free(events);
    return _exit == true ? 0 : -1;
}

int CurlEpollWorker::init_worker_info()
{
    bool ok = false;

    _epoll_fd = 0;
    _timer_fd = 0;
    _multi = NULL;

    do {
        int efd = epoll_create(_max_conn_count);
        if (efd == -1) {
            break ;
        }

        int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
        if (tfd == -1) {
            break ;
        }

        int cfds[2];
        struct itimerspec	its;
        struct epoll_event	ev;

        memset(&its, 0, sizeof(its));
        its.it_interval.tv_sec = 0;
        its.it_value.tv_sec = 1;
        if (timerfd_settime(tfd, 0, &its, NULL) < 0) {
            break ;
        }

        auto add_epoll_fd = [efd] (int fd) {
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.fd = fd;
            return epoll_ctl(efd, EPOLL_CTL_ADD, fd, &ev);
        };

        if (add_epoll_fd(tfd) != 0) {
            break ;
        }
        if (add_epoll_fd(_event_fds[SOCKET_PAIR_READ]) != 0) {
            break ;
        }

        _epoll_fd = efd;
        _timer_fd = tfd;
        _multi = curl_multi_init();

        curl_multi_setopt(_multi, CURLMOPT_SOCKETFUNCTION, CurlEpollWorker::curl_sock_cb);
        curl_multi_setopt(_multi, CURLMOPT_SOCKETDATA, this);
        curl_multi_setopt(_multi, CURLMOPT_TIMERFUNCTION, CurlEpollWorker::curl_timer_cb);
        curl_multi_setopt(_multi, CURLMOPT_TIMERDATA, this);

        ok = true;
    } while (0);

    if (ok == false) {
        release_worker_info();
    }
    return ok == true ? 0 : -1;
}

void CurlEpollWorker::release_worker_info()
{
    if (_epoll_fd != 0) {
        close(_epoll_fd);
    }
    if (_timer_fd != 0) {
        close(_timer_fd);
    }

    if (_multi != NULL) {
        curl_multi_cleanup(_multi);
    }

    for (auto& item : _relate_curls) {
        curl_easy_cleanup(item.second->handle);
        free(item.second);
    }
    for (auto& item : _unrelate_curls) {
        curl_easy_cleanup(item.first);
        free(item.second);
    }
}

void CurlEpollWorker::release_resource()
{
    int msgs_left;
    CURLMsg* msg;

    while ((msg = curl_multi_info_read(_multi, &msgs_left))) {
        if (msg->msg != CURLMSG_DONE) {
            continue ;
        }

        CurlInfo* info = NULL;

        auto handle = msg->easy_handle;
        auto result = msg->data.result;
        curl_easy_getinfo(handle, CURLINFO_PRIVATE, &info);
        if (result != 0) {
            info->cb.error_cb(
                handle, 
                curl_easy_strerror(result)
            );
        }

        {
            lock_guard<mutex> lock(_mutex);
            _relate_curls.erase(info->fd);
            _unrelate_curls.erase(handle);
        }

        auto rc = curl_multi_remove_handle(_multi, handle);
        if (rc != 0) {
			LOG("ERROR|curl_multi_remove_handle, fail, rc:%d", rc);
        }
        curl_easy_cleanup(handle);

        _engine->reduce_conn_count(_thread_no);
        free(info);
    }
}

void CurlEpollWorker::on_timer()
{
    uint64_t count = 0;
    int err = read(_timer_fd, &count, sizeof(count));
    if (err == -1 && errno == EAGAIN) {
        return ;
    }
    if (err != sizeof(count)) {
        LOG("ERROR|read(tfd)");
    }
    int run_count;
    CURLMcode rc = curl_multi_socket_action(_multi, CURL_SOCKET_TIMEOUT, 0, &run_count);
    if (rc != CURLM_OK) {
        LOG("ERROR|curl_multi_socket_action, rc:%d", rc);
    }
    release_resource();
}

void CurlEpollWorker::on_request()
{
    int ret = read(_event_fds[SOCKET_PAIR_READ], _buffer + _buf_used_size, _buf_size - _buf_used_size);
    if (ret == 0) {
        _exit = true;
        return ;
    }

    char* cur_pos = _buffer;
    char* end_pos = _buffer + _buf_used_size + ret;
    while (cur_pos < end_pos) {
        auto req = (RequestEvent*)cur_pos;
        auto rc  = curl_multi_add_handle(_multi, req->handle);
        if (rc != CURLM_OK) {

            CurlInfo* curl_info = NULL;
            {
                lock_guard<mutex> lock(_mutex);
                auto iter = _unrelate_curls.find(req->handle);
                if (iter != _unrelate_curls.end()) {
                    _unrelate_curls.erase(req->handle);
                    curl_info = iter->second;
                }
            }

            if (curl_info) {
                auto errmsg = curl_multi_strerror(rc);
                curl_info->cb.error_cb(req->handle, errmsg);
                free(curl_info);
            }
            curl_easy_cleanup(req->handle);
            _engine->reduce_conn_count(_thread_no);
        }
        cur_pos += sizeof(RequestEvent);
    }

    if (cur_pos == end_pos) {
        _buf_used_size = 0;
    } else {
        _buf_used_size = (int)(end_pos - cur_pos);
        memmove(_buffer, cur_pos, _buf_used_size);
    }
}

int CurlEpollWorker::on_event(int fd, int event)
{
    int action =
        ((event & EPOLLIN) ? CURL_CSELECT_IN : 0) |
        ((event & EPOLLOUT) ? CURL_CSELECT_OUT : 0);

    int run_count;
    CURLMcode rc = curl_multi_socket_action(_multi, fd, action, &run_count);
    if (rc != CURLM_OK) {
        LOG("ERROR|curl_multi_socket_action, error, rc:%d, fd:%d, action:%d", rc, fd, action);
    }
    release_resource();
    if (run_count <= 0) {
        struct itimerspec its;
        memset(&its, 0, sizeof(its));
        timerfd_settime(_timer_fd, 0, &its, NULL);
    }
    return 0;
}

void CurlEpollWorker::deal_sock_event(CURL* curl, curl_socket_t sock, int what)
{
    {
        lock_guard<mutex> lock(_mutex);
        if (!_is_running) {
            LOG("ERROR|_is_running is false");
            return ;
        }
    }

    if (what == CURL_POLL_REMOVE) {
        remove_sock(sock);
    } else {
        bool is_add_sock;
        {
            lock_guard<mutex> lock(_mutex);
            is_add_sock = _relate_curls.find(sock) == _relate_curls.end() ? true : false;
        }
        if (is_add_sock) {
            add_sock(sock, curl, what);
        } else {
            set_sock(sock, what);
        }
    }
}

bool CurlEpollWorker::add_sock(curl_socket_t sock, CURL* curl, int action)
{
    CurlInfo* info;
    {
        lock_guard<mutex> lock(_mutex);
        if (!_is_running) {
            LOG("ERROR|_is_running is false");
            return false;
        }
        auto iter = _unrelate_curls.find(curl);
        if (iter == _unrelate_curls.end()) {
            LOG("ERROR|_unrelate_curls not found, socket:%d, handle:%p", sock, curl);
            return false;
        }
        info = iter->second;
        info->fd = sock;
    }

    bool ret = set_sock(sock, action, true);
    {
        lock_guard<mutex> lock(_mutex);
        if (!ret) {
            free(_unrelate_curls[info->handle]);
            _unrelate_curls.erase(info->handle);
            LOG("ERROR|set_sock fail, socket:%d, handle:%p", sock, curl);
            _engine->reduce_conn_count(_thread_no);
            return false;
        } else {
            _unrelate_curls.erase(info->handle);
            _relate_curls[sock] = info;
        }
    }
    curl_multi_assign(_multi, sock, NULL);
    return true;
}

bool CurlEpollWorker::set_sock(curl_socket_t sock, int action, bool add)
{
    if (!add) {
        int ret = epoll_ctl(_epoll_fd, EPOLL_CTL_DEL, sock, NULL);
        if (ret != 0) {
            LOG("ERROR|epoll_ctl EPOLL_CTL_DEL fail, ret:%d, errno:%d", ret, errno);
            return false;
        }
    }

    struct epoll_event ev;
    ev.events = ((action & CURL_POLL_IN) ? EPOLLIN : 0) |
                ((action & CURL_POLL_OUT) ? EPOLLOUT : 0);
    ev.data.fd = sock;
    int ret = epoll_ctl(_epoll_fd, EPOLL_CTL_ADD, sock, &ev);
    if (ret != 0) {
        LOG("ERROR|epoll_ctl EPOLL_CTL_ADD fail, ret:%d, errno:%d", ret, errno);
        return false;
    }
    return true;
}

void CurlEpollWorker::remove_sock(curl_socket_t sock)
{
    epoll_ctl(_epoll_fd, EPOLL_CTL_DEL, sock, NULL);
    {
        lock_guard<mutex> lock(_mutex);
        _relate_curls.erase(sock);
    }
}

// #######################################################
//                    异步引擎功能函数
// #######################################################
CurlAsyncEngine::CurlAsyncEngine(int thread_count, int max_conn_count)
{
    _status = ENGINE_STOP;
    _thread_count   = thread_count;
    _max_conn_count = max_conn_count;
    _cur_conn_count = 0;
}

CurlAsyncEngine::~CurlAsyncEngine()
{
    if (_status == ENGINE_RUNNING) {
        stop();
    }
}

int CurlAsyncEngine::start()
{
    if (_status == ENGINE_RUNNING) {
        return -1;
    }
    LOG("start, _thread_count:%d", _thread_count);
    for (int i = 0; i < _thread_count; i++) {
        auto worker = new CurlEpollWorker(this, i, _max_conn_count);
        worker->start();
        _workers.push_back({worker, 0});
    }
    _status = ENGINE_RUNNING;
    return 0;
}

int CurlAsyncEngine::stop(bool wait)
{
    if (_status == ENGINE_STOP) {
        return -1;
    }
    for (auto& item : _workers) {
        item.worker->stop(wait);
    }
    _status = ENGINE_RUNNING;
    return 0;
}

int CurlAsyncEngine::request(CURL* handle, CurlCallback cb)
{
    int index = 0;
    {
        lock_guard<mutex> lock(_mutex);
        if (_cur_conn_count >= _max_conn_count) {
            return -1;
        }

        int min_count = -1;
        for (int i = 0; i < (int)_workers.size(); i++) {
            auto& item = _workers[i];
            if (item.worker->is_running() == false) {
                continue ;
            }
            if (min_count == -1) {
                min_count = item.cur_conn_count;
            } else if (item.cur_conn_count < min_count) {
                index = i;
                min_count = item.cur_conn_count;
            }
        }
        _workers[index].cur_conn_count++;
        _cur_conn_count++;
    }

    auto worker = _workers[index].worker;
    if (worker->request(handle, cb) != 0) {
        reduce_conn_count(index);
        return -1;
    }
    return 0;
}

void CurlAsyncEngine::reduce_conn_count(int thread_no)
{
    if (thread_no >= _workers.size()) {
        return ;
    }
    lock_guard<mutex> lock(_mutex);
    int& cur_count = _workers[thread_no].cur_conn_count;
    if (--cur_count < 0) {
        cur_count = 0;
    }
    if (--_cur_conn_count < 0) {
        _cur_conn_count = 0;
    }
}
