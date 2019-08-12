#pragma once

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>//getpid, GetTid

enum LOG_LEVEL
{
    FATAL = 1,
    ERROR,
    WARN,
    INFO,
    DEBUG,
    TRACE
};

extern pid_t GetTid();

struct UTCTimer
{
public:
    UTCTimer()
    {
        struct timeval tv;
        gettimeofday(&tv, NULL);
        //set sys_sec_, sys_min_
        sys_sec_ = tv.tv_sec;
        sys_min_ = sys_sec_ / 60;
        //use sys_sec_ to calculate year, mon, day, hour, min, sec
        struct tm cur_tm;
        localtime_r((time_t*)&sys_sec_, &cur_tm);
        year = cur_tm.tm_year + 1900;
        mon  = cur_tm.tm_mon + 1;
        day  = cur_tm.tm_mday;
        hour  = cur_tm.tm_hour;
        min  = cur_tm.tm_min;
        sec  = cur_tm.tm_sec;
        reset_utc_fmt();
    }

    uint64_t GetCurrTime(int* millisecond = NULL)
    {
        struct timeval tv;
        //get current time
        gettimeofday(&tv, NULL);
        if (millisecond != NULL)
            *millisecond = tv.tv_usec / 1000;
        //if current time is not same with sys_sec_
        if ((uint32_t)tv.tv_sec != sys_sec_)
        {
            sec = tv.tv_sec % 60;
            sys_sec_ = tv.tv_sec;
            //if current time is not is not same with sys_min_
            if (sys_sec_ / 60 != sys_min_)
            {
                //use sys_sec_ to update year, mon, day, hour, min, sec
                sys_min_ = sys_sec_ / 60;
                struct tm cur_tm;
                localtime_r((time_t*)&sys_sec_, &cur_tm);
                year = cur_tm.tm_year + 1900;
                mon  = cur_tm.tm_mon + 1;
                day  = cur_tm.tm_mday;
                hour = cur_tm.tm_hour;
                min  = cur_tm.tm_min;
                //reformat utc format
                reset_utc_fmt();
            }
            else
            {
                //reformat utc format only sec
                reset_utc_fmt_sec();
            }
        }
        return tv.tv_sec;
    }

    int year, mon, day, hour, min, sec;
    char utc_fmt_[20];

private:
    void reset_utc_fmt()
    {
        snprintf(utc_fmt_, 20, "%d-%02d-%02d %02d:%02d:%02d", year, mon, day, hour, min, sec);
    }
    
    void reset_utc_fmt_sec()
    {
        snprintf(utc_fmt_ + 17, 3, "%02d", sec);
    }

    uint64_t sys_min_;
    uint64_t sys_sec_;
};

class CellBuffer
{
public:
    enum CellBufferStatus
    {
        FREE,
        FULL
    };

    CellBuffer(uint32_t len): 
    status_(FREE), 
    prev_(NULL), 
    next_(NULL), 
    total_len_(len), 
    used_len_(0)
    {
        curr_data_ = new char[len];
        if (!curr_data_)
        {
            fprintf(stderr, "no space to allocate curr_data_\n");
            exit(1);
        }
    }

    uint32_t AvailLength() const { return total_len_ - used_len_; }

    bool Empty() const { return used_len_ == 0; }

    void Append(const char* log_line, uint32_t len)
    {
        if (AvailLength() < len)
            return ;
        memcpy(curr_data_ + used_len_, log_line, len);
        used_len_ += len;
    }

    void Clear()
    {
        used_len_ = 0;
        status_ = FREE;
    }

    void Persist(FILE* fp)
    {
        uint32_t wt_len = fwrite(curr_data_, 1, used_len_, fp);
        if (wt_len != used_len_)
        {
            fprintf(stderr, "write log to disk error, wt_len %u\n", wt_len);
        }
    }

    CellBufferStatus status_;

    CellBuffer* prev_;
    CellBuffer* next_;

private:
    CellBuffer(const CellBuffer&);
    CellBuffer& operator=(const CellBuffer&);

    uint32_t total_len_;
    uint32_t used_len_;
    char* curr_data_;
};

class ElasticLog
{
public:
    // singleton
    static ElasticLog* Instance()
    {
        pthread_once(&init_once_, ElasticLog::InitInstance);
        return instance_;
    }

    static void InitInstance()
    {
        while (!instance_) 
            instance_ = new ElasticLog();
    }

    void InitLogPath(const char* log_dir, const char* prog_name, int level);

    int GetLogLevel() const { return log_level_; }

    void Persist();

    void TryAppendLogEntry(const char* lvl, const char* format, ...);

private:
    ElasticLog();

    bool TargetLogPathLegal(int year, int mon, int day);

    ElasticLog(const ElasticLog&);
    const ElasticLog& operator=(const ElasticLog&);

    int buffer_count_;

    CellBuffer* curr_buffer_;
    CellBuffer* persist_buffer_;

    FILE* fp_;
    pid_t pid_;
    int year_, mon_, day_, log_count_;
    char program_name_[256];
    char log_dir_[256];

    bool log_path_legal_; //if log dir legal
    int log_level_;
    uint64_t last_log_failure_ts_; //last can't log error time(s) if value != 0, log error happened last time
    
    UTCTimer tm_;

    static pthread_mutex_t mutex_;
    static pthread_cond_t cond_;

    static uint32_t cell_buffer_len_;

    // singleton
    static ElasticLog* instance_;
    static pthread_once_t init_once_;
};

void* persist_thread(void* args);

#define LOG_INIT(log_dir, prog_name, level) \
    ElasticLog::Instance()->InitLogPath(log_dir, prog_name, level); \
    pthread_t tid; \
    pthread_create(&tid, NULL, persist_thread, NULL); \
    pthread_detach(tid);

//format: [LEVEL][yy-mm-dd h:m:s.ms][tid]file_name:line_no(func_name):content
#define LOG_TRACE(fmt, args...) \
    ElasticLog::Instance()->TryAppendLogEntry("[TRACE]", "[%u]%s:%d(%s): " fmt "\n", \
            GetTid(), __FILE__, __LINE__, __FUNCTION__, ##args); 


#define LOG_DEBUG(fmt, args...) \
    ElasticLog::Instance()->TryAppendLogEntry("[DEBUG]", "[%u]%s:%d(%s): " fmt "\n", \
            GetTid(), __FILE__, __LINE__, __FUNCTION__, ##args);

#define LOG_INFO(fmt, args...) \
    ElasticLog::Instance()->TryAppendLogEntry("[INFO]", "[%u]%s:%d(%s): " fmt "\n", \
            GetTid(), __FILE__, __LINE__, __FUNCTION__, ##args); 

#define LOG_NORMAL(fmt, args...) \
    ElasticLog::Instance()->TryAppendLogEntry("[INFO]", "[%u]%s:%d(%s): " fmt "\n", \
            GetTid(), __FILE__, __LINE__, __FUNCTION__, ##args);

#define LOG_WARN(fmt, args...) \
    ElasticLog::Instance()->TryAppendLogEntry("[WARN]", "[%u]%s:%d(%s): " fmt "\n", \
            GetTid(), __FILE__, __LINE__, __FUNCTION__, ##args);

#define LOG_ERROR(fmt, args...) \
    ElasticLog::Instance()->TryAppendLogEntry("[ERROR]", "[%u]%s:%d(%s): " fmt "\n", \
        GetTid(), __FILE__, __LINE__, __FUNCTION__, ##args); 

#define LOG_FATAL(fmt, args...) \
    ElasticLog::Instance()->TryAppendLogEntry("[FATAL]", "[%u]%s:%d(%s): " fmt "\n", \
        GetTid(), __FILE__, __LINE__, __FUNCTION__, ##args); 
