#include <windows.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <cstdio>
#include <thread>
using namespace std;

static mutex g_mutex;
static condition_variable g_cv;
static queue<string> g_q;

static long long NowMs() { return chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now().time_since_epoch()).count(); }

int main()
{
    // t1: mimics the server main thread in WaitForPlayerInput
    thread t1([&] {
        auto lastBeat = chrono::steady_clock::now();
        while (true)
        {
            auto now = chrono::steady_clock::now();
            if (now - lastBeat >= chrono::seconds(2))
            {
                lastBeat = now;
                printf("[t1] BEAT %lld q=%d\n", NowMs(), (int)g_q.size());
            }
            unique_lock<mutex> lock(g_mutex);
            if (g_cv.wait_for(lock, chrono::milliseconds(100)) == cv_status::no_timeout)
            {
                if (!g_q.empty())
                {
                    g_q.pop();
                    printf("[t1] POP %lld\n", NowMs());
                }
            }
        }
    });

    // t2: mimics the recv thread - pushes one msg at t=5s, one at t=65s
    thread t2([&] {
        long long t0 = NowMs();
        while (true)
        {
            Sleep(500);
            long long now = NowMs();
            if (now - t0 >= 5000 && now - t0 < 6500 && (now / 500) % 2 == 1)
            {
                {
                    lock_guard<mutex> lock(g_mutex);
                    if (g_q.empty()) g_q.push("x");
                }
                g_cv.notify_all();
            }
            if (now - t0 >= 65000 && now - t0 < 66000 && (now / 500) % 2 == 1)
            {
                {
                    lock_guard<mutex> lock(g_mutex);
                    if (g_q.empty()) g_q.push("x");
                }
                g_cv.notify_all();
            }
            if (now - t0 > 175000) break;
        }
    });

    // t0: control - 1s wall-clock probe, steady deltas
    thread t0([&] {
        long long t0m = NowMs();
        while (true)
        {
            Sleep(1000);
            long long now = NowMs();
            long long wall = (long long)time(nullptr);
            printf("[t0] +%lldms wall=%lld\n", now - t0m, wall);
            if (now - t0m > 95000) break;
        }
    });

    t1.join(); t2.join(); t0.join();
    printf("probe done\n");
    return 0;
}
