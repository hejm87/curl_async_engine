#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <sys/time.h>
#include <sys/types.h>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include "curl/multi.h"
#include "curl_worker.h"
#include "utils.h"

using namespace std;

const string g_url = "http://127.0.0.1:8000";

string g_result;
map<string, string> g_results;

mutex g_mutex;
int g_counter;
int g_tickets;

void read_sample_cb(CURL* handle, void* ptr, size_t size, size_t nmemb)
{
	string str = (char*)ptr;
	g_result += str;
}

void read_concurrency_cb(CURL* handle, void* ptr, size_t size, size_t nmemb)
{
	string str = (char*)ptr;
	lock_guard<mutex> lock(g_mutex);
	g_results[str] = "1";
}

void read_qps_cb(CURL* handle, void* ptr, size_t size, size_t nmemb)
{
	lock_guard<mutex> lock(g_mutex);
	g_counter++;  
	g_tickets++;
}

void error_cb(CURL* handle, const string& error)
{
	LOG(">>>>>>>>>>>> http.error, handle:%p, error:%s", handle, error.c_str());
}

CURL* new_curl_handle(const string& url)
{
	CURL* handle = curl_easy_init();
	assert(handle != NULL);
	curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
	return handle;
}

int test_sample(CurlAsyncEngine* engine);
int test_concurrency(CurlAsyncEngine* engine);
int test_qps(CurlAsyncEngine* engine);
int test_performance(CurlAsyncEngine* engine);

int main(int argc, char** argv)
{
	// printf("curl async engine starting ...\n");
	auto engine = new CurlAsyncEngine(2, 5000);

	int ret = engine->start();
	assert(ret == 0);
	// time to run worker thread
	usleep(100 * 1000);

	// test_sample(engine);
	// test_concurrency(engine);
	test_qps(engine);
	// test_performance(engine);

	ret = engine->stop();
	assert(ret == 0);

	return 0;
}

int test_sample(CurlAsyncEngine* engine)
{
	g_result = "";
	for (int i = 0; i < 3; i++) {

		CurlCallback cb;
		cb.read_cb  = read_sample_cb;
		cb.error_cb = error_cb;

		auto handle = new_curl_handle(g_url + "?echo=" + to_string(i));
		int ret = engine->request(handle, cb);
		assert(ret == 0);

		usleep(100 * 1000);
	}
	assert(g_result == "012");
	return 0;
}

int test_concurrency(CurlAsyncEngine* engine)
{
	g_results.clear();

	int loop = 5000;
	vector<string> keys;

	long beg_ms = now_ms();
	for (int i = 0; i < loop; i++) {

		CurlCallback cb;
		cb.read_cb  = read_concurrency_cb;
		cb.error_cb = error_cb;

		string echo = "concurrency" + to_string(i);
		keys.push_back(echo);

		auto handle = new_curl_handle(g_url + "?echo=" + echo);
		int ret = engine->request(handle, cb);
		if (ret != 0) {
			LOG(">>>>>>>>>> ERROR, request fail, echo:%s", echo.c_str());
		}
		assert(ret == 0);
	}

	int mid_finish_count = 0;
	{
		lock_guard<mutex> lock(g_mutex);
		mid_finish_count = g_results.size();
	}

	long now_ms1 = now_ms();
	LOG("request %d count, mid_finish_count:%d, cost:%ldms", loop, mid_finish_count, now_ms1 - beg_ms);

	int timeout = 0;
	{
		long beg_ms = now_ms();
		while (1) {
			long now_ms = now_ms();
			if (now_ms - beg_ms >= 5000) {
				timeout = 1;
				break ;
			}
			map<string, string> results;
			{
				lock_guard<mutex> lock(g_mutex);
				results = g_results;
			}
			if (results.size() == loop) {
				for (auto item : keys) {
					assert(results.find(item) != results.end());
				}
				break ;
			}
			usleep(1);
		}
	}

	long now_ms2 = now_ms();
	LOG("finish %d count, timeout:%d, cost:%ldms", loop, timeout, now_ms2 - beg_ms);

	return 0;
}

int test_qps(CurlAsyncEngine* engine)
{
	g_counter = 0;
	g_tickets = 5000;

	int thread_count = 2;
	// create n producer to request
	for (int i = 0; i < thread_count; i++) {
		auto t = thread([engine, i]() {
			LOG("test_thread[%d]:%u start", i, gettid());
			int req_no = 0;
			while (1) {

				bool do_request = false;
				{
					lock_guard<mutex> lock(g_mutex);
					if (g_tickets > 0) {
						g_tickets--;      
						do_request = true;
					}
				}

				if (do_request == false) {
					usleep(1);
					continue ;
				}

				CurlCallback cb;
				cb.read_cb  = read_qps_cb;
				cb.error_cb = error_cb;

				string echo = "qps_" + to_string(i) + "_" + to_string(req_no++);

				long beg_ms = now_ms();
				auto handle = new_curl_handle(g_url + "?echo=" + echo);
				int ret = engine->request(handle, cb);
				if (ret != 0) {
					LOG("ERROR|request fail, echo:%s", echo.c_str());
				}
				assert(ret == 0);
				long end_ms = now_ms();
				if (end_ms - beg_ms > 5) {
					;
				}
			}
		});
		t.detach();
	}

	long last_counter = g_counter;
	while (1) {
		sleep(1);
		int qps = 0;
		{
			lock_guard<mutex> lock(g_mutex);
			qps = g_counter - last_counter;
			last_counter = g_counter;
		}
		LOG("qps:%d", qps);
	}

	return 0;
}
