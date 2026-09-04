#include <thread>
#include <cstdio>
int main() {
    std::thread t([]{ std::printf("hello from thread\n"); std::fflush(stdout); });
    t.join();
    std::printf("joined, done\n");
    std::fflush(stdout);
    return 0;
}
