#ifndef __CURL_ASYNC_WORKER_H__
#define __CURL_ASYNC_WORKER_H__

#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include <map>
#include <vector>
#include <string>

#include "curl/curl.h"
#include "curl/multi.h"

using namespace std;

typedef function<void(CURL*, void*, size_t, size_t)>    read_callback;
typedef function<void(CURL*, const string&)>   error_callback;

struct CurlInfo;

struct CurlCallback
{
	read_callback  read_cb;
	error_callback  error_cb;
};

// ###########################################################
//                       CurlEpollWorker
// ###########################################################
class CurlAsyncEngine;

class CurlEpollWorker
{
public:
	CurlEpollWorker(CurlAsyncEngine* engine, int thread_no, int max_conn_count);
	~CurlEpollWorker();

	void set_thread_name(const string& thread_name) {
		_thread_name = thread_name;
	}

	bool is_running() {
		lock_guard<mutex> lock(_mutex);
		return _is_running;
	}

	int  start();
	void stop(bool wait = true);

	int request(CURL* handle, CurlCallback cb);

private:
	int thread_run();

	int  init_worker_info();
	void release_worker_info();

	void release_resource();

	void on_timer();
	void on_request();
	int  on_event(int fd, int event);

	void deal_sock_event(CURL* curl, curl_socket_t sock, int what);

	bool set_sock(curl_socket_t sock, int action, bool add = false);
	bool add_sock(curl_socket_t sock, CURL* curl, int action);
	void remove_sock(curl_socket_t sock);

	static int curl_timer_cb(CURLM* multi, long timeout_ms, void* data);
	static int curl_sock_cb(CURL* curl, curl_socket_t sock, int what, void* cbp, void* sockp);
	static size_t curl_read_cb(void* ptr, size_t size, size_t nmemb, void* data);

private:
	string _thread_name;

	int  _thread_no;

	bool _exit;
	bool _is_running;
	int  _max_conn_count;

	int  _epoll_fd;
	int  _timer_fd;
	int  _event_fds[2];

	int  _buf_size;
	int  _buf_used_size;
	char* _buffer;

	CURLM* _multi;

	mutex _mutex;

	map<int, CurlInfo*>  _relate_curls;
	map<CURL*, CurlInfo*> _unrelate_curls;

	thread _thread;

	CurlAsyncEngine* _engine;
};

// ###########################################################
//                      CurlAsyncWorker
// ###########################################################
struct CurlEpollWorkerInfo
{
	CurlEpollWorker* worker;
	int cur_conn_count;
};

class CurlAsyncEngine
{
	friend class CurlEpollWorker;
	public:
	CurlAsyncEngine(int thread_count, int max_conn_count);
	~CurlAsyncEngine();

	void set_thread_count(int thread_count) {
		_thread_count = thread_count;
	}

	int start();
	int stop(bool wait = true);

	int request(CURL* handle, CurlCallback cb);

	private:
	void reduce_conn_count(int thread_no);

	private:
	enum {
		ENGINE_STOP = 0,
		ENGINE_RUNNING = 1,
	};

	int _status;
	int _thread_count;
	int _max_conn_count;
	int _cur_conn_count;

	vector<CurlEpollWorkerInfo> _workers;

	mutex _mutex;
};

#endif
