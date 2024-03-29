#include "elasticlog.h"
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <pthread.h>

int64_t get_current_millis(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void* func(void *)
{
    for (int i = 0; i < 1e6; ++i)
    {
        LOG_ERROR("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx. index of the log entry is %d", i);
    }
}

int main(int argc, char** argv)
{
    LOG_INIT("log", "log_name", 3);
    uint64_t start_ts = get_current_millis();
    pthread_t tids[5];
    for (int i = 0; i < 5; ++i)
    	pthread_create(&tids[i], NULL, func, NULL);

    for (int i = 0; i < 5; ++i)
	    pthread_join(tids[i], NULL);
    uint64_t end_ts = get_current_millis();
    printf("time use %lu ms\n", end_ts - start_ts);
}
