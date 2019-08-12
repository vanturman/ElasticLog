#include "elasticlog.h"

#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/syscall.h>

#define MEM_USE_LIMIT (3u * 1024 * 1024 * 1024)
#define SINGLE_LOG_SIZE_LIMIT (1u * 1024 * 1024 * 1024)
#define LOG_LEN_LIMIT (1 * 1024)
#define TIME_TO_WAIT 3
#define PERSIST_SLEEP_TIME 1

pid_t GetTid()
{
    return syscall(__NR_gettid);
}

pthread_mutex_t ElasticLog::mutex_ = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ElasticLog::cond_ = PTHREAD_COND_INITIALIZER;

ElasticLog* ElasticLog::instance_ = NULL;
pthread_once_t ElasticLog::init_once_ = PTHREAD_ONCE_INIT;
uint32_t ElasticLog::cell_buffer_len_ = 30*1024*1024;  //30MB

ElasticLog::ElasticLog():
    buffer_count_(3),
    curr_buffer_(NULL),
    persist_buffer_(NULL),
    fp_(NULL),
    log_count_(0),
    log_path_legal_(false),
    log_level_(INFO),
    last_log_failure_ts_(0),
    tm_()
{
    /* Initialize double linked cell buffer list */
    CellBuffer* head = new CellBuffer(cell_buffer_len_);
    if (!head)
    {
        fprintf(stderr, "no space to allocate CellBuffer\n");
        exit(1);
    }
    CellBuffer* current;
    CellBuffer* prev_ = head;
    for (int i = 1; i < buffer_count_; ++i)
    {
        current = new CellBuffer(cell_buffer_len_);
        if (!current)
        {
            fprintf(stderr, "no space to allocate CellBuffer\n");
            exit(1);
        }
        current->prev_ = prev_;
        prev_->next_ = current;
        prev_ = current;
    }
    prev_->next_ = head;
    head->prev_ = prev_;

    curr_buffer_ = head;
    persist_buffer_ = head;

    pid_ = getpid();
}

void ElasticLog::InitLogPath(const char* log_dir, const char* program_name, int level)
{
    pthread_mutex_lock(&mutex_);

    /* name format:  name_yearmonday.tid.log.n */
    strncpy(log_dir_, log_dir, 256);
    strncpy(program_name_, program_name, 256);
    mkdir(log_dir_, 0777);
    // file exists or have the permission to write log file
    if (access(log_dir_, F_OK | W_OK) == -1)
    {
        fprintf(stderr, "log dir: %s error: %s\n", log_dir_, strerror(errno));
    }
    else
    {
        log_path_legal_ = true;
    }
    level = level > TRACE ? TRACE : level;
    level = level < FATAL ? FATAL : level;
    log_level_ = level;

    pthread_mutex_unlock(&mutex_);
}

void ElasticLog::Persist()
{
    while (true)
    {
        // check if persist_buffer_ need to be persist
        pthread_mutex_lock(&mutex_);
        if (persist_buffer_->status_ == CellBuffer::FREE)
        {
            struct timespec tsp;
            struct timeval now;
            gettimeofday(&now, NULL);
            tsp.tv_sec = now.tv_sec;
            tsp.tv_nsec = now.tv_usec * 1000;   //nanoseconds
            tsp.tv_sec += PERSIST_SLEEP_TIME;   //wait for 1 seconds
            pthread_cond_timedwait(&cond_, &mutex_, &tsp);
        }
        if (persist_buffer_->Empty())
        {
            // try again
            pthread_mutex_unlock(&mutex_);
            continue;
        }

        if (persist_buffer_->status_ == CellBuffer::FREE)
        {
            assert(curr_buffer_ == persist_buffer_);
            curr_buffer_->status_ = CellBuffer::FULL;
            curr_buffer_ = curr_buffer_->next_;
        }

        /* Persist Workloads */
        int year = tm_.year, mon = tm_.mon, day = tm_.day;
        pthread_mutex_unlock(&mutex_);

        //decision which file to write
        if (!TargetLogPathLegal(year, mon, day))
            continue;
        persist_buffer_->Persist(fp_);
        fflush(fp_);

        pthread_mutex_lock(&mutex_);
        persist_buffer_->Clear();
        persist_buffer_ = persist_buffer_->next_;
        pthread_mutex_unlock(&mutex_);
    }
}

void ElasticLog::TryAppendLogEntry(const char* log_level, const char* format, ...)
{
    int millisecond;
    uint64_t curr_sec = tm_.GetCurrTime(&millisecond);
    if (last_log_failure_ts_ && curr_sec - last_log_failure_ts_ < TIME_TO_WAIT)
        return ;

    char log_line[LOG_LEN_LIMIT];
    int prev_len = snprintf(log_line, LOG_LEN_LIMIT, "%s[%s.%03d]", log_level, tm_.utc_fmt_, millisecond);

    va_list arg_ptr;
    va_start(arg_ptr, format);
    int main_len = vsnprintf(log_line + prev_len, LOG_LEN_LIMIT - prev_len, format, arg_ptr);
    va_end(arg_ptr);

    uint32_t log_len = prev_len + main_len;

    last_log_failure_ts_ = 0;
    bool produce_flag = false;

    pthread_mutex_lock(&mutex_);
    if (curr_buffer_->status_ == CellBuffer::FREE && curr_buffer_->AvailLength() >= log_len)
    {
        curr_buffer_->Append(log_line, log_len);
    }
    else
    {   
        // current buffer can't be written under two scenario
        // 1. curr_buffer_->status_ = CellBuffer::FREE but curr_buffer_->AvailLength() < log_len
        // 2. curr_buffer_->status_ = CellBuffer::FULL
        if (curr_buffer_->status_ == CellBuffer::FREE)
        {
            curr_buffer_->status_ = CellBuffer::FULL;
            CellBuffer* next_buffer = curr_buffer_->next_;
            produce_flag = true;

            // this buffer is under the Persist job
            if (next_buffer->status_ == CellBuffer::FULL)
            {
                // if memory use < MEM_USE_LIMIT, allocate new CellBuffer
                if (cell_buffer_len_ * (buffer_count_ + 1) > MEM_USE_LIMIT)
                {
                    fprintf(stderr, "no more log space can use\n");
                    curr_buffer_ = next_buffer;
                    last_log_failure_ts_ = curr_sec;
                }
                else
                {
                    CellBuffer* new_buffer = new CellBuffer(cell_buffer_len_);
                    buffer_count_ += 1;
                    new_buffer->prev_ = curr_buffer_;
                    curr_buffer_->next_ = new_buffer;
                    new_buffer->next_ = next_buffer;
                    next_buffer->prev_ = new_buffer;
                    curr_buffer_ = new_buffer;
                }
            }
            else
            {
                // next_ buffer is free 
                curr_buffer_ = next_buffer;
            }
            if (!last_log_failure_ts_)
                curr_buffer_->Append(log_line, log_len);
        }
        else
        {
            last_log_failure_ts_ = curr_sec;
        }
    }
    
    if (produce_flag)
    {
        pthread_cond_signal(&cond_);
    }
    pthread_mutex_unlock(&mutex_);
}

bool ElasticLog::TargetLogPathLegal(int year, int mon, int day)
{
    if (!log_path_legal_)
    {
        if (fp_)
            fclose(fp_);
        fp_ = fopen("/dev/null", "w");
        return fp_ != NULL;
    }
    if (!fp_)
    {
        year_ = year, mon_ = mon, day_ = day;
        char log_path[1024] = {};
        sprintf(log_path, "%s/%s.%d%02d%02d.%u.log", log_dir_, program_name_, year_, mon_, day_, pid_);
        fp_ = fopen(log_path, "w");
        if (fp_)
            log_count_ += 1;
    }
    else if (day_ != day)
    {
        fclose(fp_);
        char log_path[1024] = {};
        year_ = year, mon_ = mon, day_ = day;
        sprintf(log_path, "%s/%s.%d%02d%02d.%u.log", log_dir_, program_name_, year_, mon_, day_, pid_);
        fp_ = fopen(log_path, "w");
        if (fp_)
            log_count_ = 1;
    }
    else if (ftell(fp_) >= SINGLE_LOG_SIZE_LIMIT)
    {
        fclose(fp_);
        char old_path[1024] = {};
        char new_path[1024] = {};
        //mv xxx.log.[i] xxx.log.[i + 1]
        for (int i = log_count_ - 1;i > 0; --i)
        {
            sprintf(old_path, "%s/%s.%d%02d%02d.%u.log.%d", log_dir_, program_name_, year_, mon_, day_, pid_, i);
            sprintf(new_path, "%s/%s.%d%02d%02d.%u.log.%d", log_dir_, program_name_, year_, mon_, day_, pid_, i + 1);
            rename(old_path, new_path);
        }
        //mv xxx.log xxx.log.1
        sprintf(old_path, "%s/%s.%d%02d%02d.%u.log", log_dir_, program_name_, year_, mon_, day_, pid_);
        sprintf(new_path, "%s/%s.%d%02d%02d.%u.log.1", log_dir_, program_name_, year_, mon_, day_, pid_);
        rename(old_path, new_path);
        fp_ = fopen(old_path, "w");
        if (fp_)
            log_count_ += 1;
    }
    return fp_ != NULL;
}

void* persist_thread(void* args)
{
    ElasticLog::Instance()->Persist();
    return NULL;
}
