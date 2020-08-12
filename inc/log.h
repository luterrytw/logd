#ifndef __LOG_H__
#define __LOG_H__

#ifdef WIN32
	#ifndef PATH_MAX
	#define PATH_MAX 260
	#endif
#else
	#include <linux/limits.h>
#endif


#define STR_HELPER(x) #x
#define TO_STR(x) STR_HELPER(x)

#define DEF_LOGD_PORT		9278
#define LOGD_MAX_BUF_LEN	2048

#ifndef LOG_TAG
	#define LOG_TAG	""
#endif

#ifdef WIN32
	#define FILE_SEPARATOR		'\\'
	#define FILE_NEWLINE		"\r\n"
#else
	#define FILE_SEPARATOR		'/'
	#define FILE_NEWLINE		"\n"
#endif

#define LOGE(fmt, ...) do { send2logd(LOG_TAG, LOG_ERROR, fmt, ##__VA_ARGS__); } while (0)
#define LOGI(fmt, ...) do { send2logd(LOG_TAG, LOG_NORMAL, fmt, ##__VA_ARGS__); } while (0)
#define LOGD(fmt, ...) do { send2logd(LOG_TAG, LOG_DEBUG, fmt, ##__VA_ARGS__); } while (0)

typedef enum LogLevel {
	LOG_FATAL,
	LOG_ALARM,
	LOG_ERROR,
	LOG_WARNING,
	LOG_NORMAL,
	LOG_DEBUG
} LogLevel;

typedef void* (*file_monitor_fn)(void* arg);
typedef struct {
	char file[PATH_MAX];
	file_monitor_fn fun;
	int running;
} file_monitor_st;

#ifdef __cplusplus
extern "C" {
#endif

void send2logd(char *tag, int level, char *fmt, ...);
int uninit_log();

#ifdef __cplusplus
}
#endif

#endif