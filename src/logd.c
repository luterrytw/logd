#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <stdint.h>
#include <sys/stat.h>

#include "udp-socket.h"

#include <unistd.h>
#include <dirent.h>

#include "logd.h"

typedef void (*file_it)(char *filename, void *user_data);

typedef struct {
	int counter;
	time_t firstDatetime; // the date time long of first log file
	time_t lastDatetime; // the date long of last log file
} LOG_FILE_INFO;

typedef struct {
	char logPath[PATH_MAX];
	int maxLogNum;
	int maxLogSize;
	int logdPort;
	int flushTime; // sec., timeout time to do flush log
	int maxMsgSize;
} LOGD_CONFIG;

typedef int (*task_act)(void *user_data);

typedef struct TaskST {
	task_act action;
	void *user_data;
	int64_t timeout; // timeout time from 1970
	struct TaskST* next;
} Task;

typedef struct {
	int isRun;
	int logSize; // bytes
	FILE* logFP;
	int listenSocket;
	int taskClientSocket;
	struct addrinfo *taskServerAddr;
	int taskServerSocket;
	Task *taskList;
	int flushLogSize;
} LOGD_STATUS;

static LOGD_CONFIG g_logdConfig;
static LOGD_STATUS g_logdStatus;
static char* g_buffer = NULL;

////////////////////////////////////////////////////////////////////////////////
// Log Rotation
////////////////////////////////////////////////////////////////////////////////
static int iterator_file(char *path, file_it fun, void *user_data)
{
    DIR *dir;
    struct dirent *file;

    dir = opendir(path);
    if (dir) {
        while ((file = readdir(dir)) != NULL) {
			/*if (file->d_type != DT_REG) {
				continue;
			}*/
            fun(file->d_name, user_data);
        }
        closedir(dir);
    }
	
	return 0;
}

/*
	datetimeStr:
		YYYYMMDD_hhmmss
*/
static time_t datetime2long(char* datetimeStr)
{
	char numStr[8];
	struct tm tm_info;

	memset(&tm_info, 0, sizeof(tm_info));
	// year
	strncpy(numStr, datetimeStr, 4);
	numStr[4] = '\0';
	tm_info.tm_year = strtol(numStr, NULL, 10) - 1900;
	// mon
	strncpy(numStr, datetimeStr+4, 2); // YYYY | MMDD_hhmmss
	numStr[2] = '\0';
	tm_info.tm_mon = strtol(numStr, NULL, 10) - 1;
	// day
	strncpy(numStr, datetimeStr+6, 2); // YYYYMM | DD_hhmmss
	numStr[2] = '\0';
	tm_info.tm_mday = strtol(numStr, NULL, 10);
	// hour
	strncpy(numStr, datetimeStr+9, 2); // YYYYMMDD_ | hhmmss
	numStr[2] = '\0';
	tm_info.tm_hour = strtol(numStr, NULL, 10);
	// min
	strncpy(numStr, datetimeStr+11, 2); // YYYYMMDD_hh | mmss
	numStr[2] = '\0';
	tm_info.tm_min = strtol(numStr, NULL, 10);
	// sec
	strncpy(numStr, datetimeStr+13, 2); // YYYYMMDD_hhmm | ss
	numStr[2] = '\0';
	tm_info.tm_sec = strtol(numStr, NULL, 10);

	return mktime(&tm_info);
}

static void log_file_info(char* name, void *user_data)
{
	LOG_FILE_INFO *info = (LOG_FILE_INFO*) user_data;
	char datetimeStr[16];
	time_t logDatetime;
	
	if (strstr(name, "_logd.txt") == NULL || strlen(name) != 24) { // 24, 20200227_085653_logd.txt
		return;
	}

	info->counter++;
	// get date time str
	strncpy(datetimeStr, name, 15);
	datetimeStr[15] = '\0';

	// parsing date string: Ymd HMS
	logDatetime = datetime2long(datetimeStr);
	if (info->firstDatetime > logDatetime) {
		info->firstDatetime = logDatetime;
	}
	if (info->lastDatetime < logDatetime) {
		info->lastDatetime = logDatetime;
	}
}

static void get_datetime_str(time_t timer, char *datetimeStr)
{
    struct tm *tmCur;

    tmCur = localtime(&timer);

	strftime(datetimeStr, 16, "%Y%m%d_%H%M%S", tmCur);
}

#ifdef DEBUG
static void dump_log_file_info(LOG_FILE_INFO *info)
{
	LOGDD("counter=%d\n", info->counter);
	LOGDD("firstDatetime=%lu\n", info->firstDatetime);
	LOGDD("lastDatetime=%lu\n", info->lastDatetime);
}
#endif

static int mkdir_rec(char *path, int mode)
{
	char parent[PATH_MAX];
	char *ptr;

	if (mkdir(path, mode) == -1) {
		if (errno == EEXIST) { // file exist
			return 0;
		} else if (errno != ENOENT) {
			return -1;
		}
		// parent doesn't exist, create it
		ptr = strrchr(path, FILE_SEPARATOR);
		if (!ptr) {
			return -1;
		}
		memcpy(parent, path, ptr-path);
		parent[ptr-path] = '\0';
		if (mkdir_rec(parent, mode)) {
			return -1;
		}
		// mkdir again
		return mkdir(path, mode);
	}
	
	return 0;
}

static int log_rotation()
{
	char datetimeStr[16]; // 20200227_085653
	char nextLogFile[PATH_MAX]; // 20200227_085653_logd.txt
	char firstLogFile[PATH_MAX]; // 20200227_085653_logd.txt
	time_t curDatetime = time(NULL);
	LOG_FILE_INFO info;
	
	firstLogFile[0] = nextLogFile[0] = '\0';
	memset(&info, 0, sizeof(info));
	info.firstDatetime = curDatetime;
	iterator_file(g_logdConfig.logPath, log_file_info, &info);
#ifdef DEBUG
	dump_log_file_info(&info);
#endif
	
	// make next log file
	get_datetime_str(curDatetime, datetimeStr); // get current date time string
	snprintf(nextLogFile, sizeof(nextLogFile)-1, "%.4069s%c%.15s_logd.txt", g_logdConfig.logPath, FILE_SEPARATOR, datetimeStr);
	nextLogFile[PATH_MAX-1] = '\0';
	
	// make first log file
	if (info.counter >= DEF_MAX_LOG_NUM) {
		get_datetime_str(info.firstDatetime, datetimeStr); // get first date time string
		snprintf(firstLogFile, sizeof(firstLogFile)-1, "%.4069s%c%.15s_logd.txt", g_logdConfig.logPath, FILE_SEPARATOR, datetimeStr);
		firstLogFile[PATH_MAX-1] = '\0';
	}

	// remove first log file
	LOGDD("try to remove(%s)", firstLogFile);
	if (firstLogFile[0] != '\0' && remove(firstLogFile)) {
		LOGDD("Remove %s fail, return\n", firstLogFile);
		return -1;
	}

	// close old log file
	if (g_logdStatus.logFP != NULL) {
		fclose(g_logdStatus.logFP);
		g_logdStatus.logFP = NULL;
	}
	// open new log file
	g_logdStatus.logFP = fopen(nextLogFile, "w");
	LOGDD("try to open(%s)", nextLogFile);
	if (g_logdStatus.logFP == NULL) {
		LOGDD("open %s fail, errno=%d\n", nextLogFile, errno);
		return -1;
	}
	g_logdStatus.logSize = 0;
	
	return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Logd Config
////////////////////////////////////////////////////////////////////////////////
#ifdef WIN32

#include <windows.h>
int util_module_path_get(char * moudlePath)
{
	int iRet = 0;
	char * lastSlash = NULL;
	char tempPath[512] = {0};
	if(NULL == moudlePath) return iRet;
	if(ERROR_SUCCESS != GetModuleFileName(NULL, tempPath, sizeof(tempPath)))
	{
		lastSlash = strrchr(tempPath, FILE_SEPARATOR);
		if(NULL != lastSlash)
		{
			strncpy(moudlePath, tempPath, lastSlash - tempPath + 1);
			iRet = lastSlash - tempPath + 1;
			moudlePath[iRet] = 0;
		}
	}
	return iRet;
}

#else // !windows
int util_module_path_get(char * moudlePath)
{
	strcpy(moudlePath,".");
	return 0;
}
#endif

static char *ltrim(char *str, const char *seps)
{
    size_t totrim;
    if (seps == NULL) {
        seps = "\t\n\v\f\r ";
    }
    totrim = strspn(str, seps);
    if (totrim > 0) {
        size_t len = strlen(str);
        if (totrim == len) {
            str[0] = '\0';
        }
        else {
            memmove(str, str + totrim, len + 1 - totrim);
        }
    }
    return str;
}

static char *rtrim(char *str, const char *seps)
{
    int i;
    if (seps == NULL) {
        seps = "\t\n\v\f\r ";
    }
    i = strlen(str) - 1;
    while (i >= 0 && strchr(seps, str[i]) != NULL) {
        str[i] = '\0';
        i--;
    }
    return str;
}

static char *trim(char *str, const char *seps)
{
    return ltrim(rtrim(str, seps), seps);
}

static char* get_config_value(char *line, const char *key)
{
	char *linePtr = line;
	const char *keyPtr = key;

	// skip space
	while (*linePtr != '\0') {
		if (*linePtr != ' ' && *linePtr != '\t') {
			break;
		}
		linePtr++;
	}
	// check key string
	while (*linePtr != '\0') {
		if (*keyPtr == '\0') {
			break;
		}
		if (*linePtr != *keyPtr) {
			return NULL;
		}
		linePtr++;
		keyPtr++;
	}
	// find '='
	while (*linePtr != '\0') {
		if (*linePtr == '=') {
			linePtr++;
			break;
		}
		linePtr++;
	}
	return trim(linePtr, NULL);
}

#define LOGD_INI_GROUP	"[LogServer]"

static int append_empty_logini(char* filename)
{
	FILE *file = fopen(filename, "a");

	LOGDD("append_empty_logini");
	if (!file) {
		return -1;
	}

	fputs("\n" LOGD_INI_GROUP "\n", file);
	fputs("#log_path=exe_path/logs\n\n", file);
	fputs("#the max log number to rotation\n", file);
	fputs("#max_log_num=10\n\n", file);
	fputs("#the max size per log file in bytes, 10485760 = 10M = 10*1024*1024\n", file);
	fputs("#max_log_size=10485760\n\n", file);
	fputs("#logd_port=9278\n\n", file);
	fputs("#sec., for debugging, you can set flush time to 1sec.\n", file);
	fputs("#flush_time=60\n\n", file);
	fputs("#max length per message.\n", file);
	fprintf(file, "#max_msg_size=%d\n\n", LOGD_MAX_BUF_LEN);

	fclose(file);
	return 0;
}

static int read_log_config(char* filename)
{
	char line[128];
	FILE *file;
	char *ptr;
	int isValid = 0;

	LOGDD("read_log_config(%s)", filename);
	file = fopen(filename, "r");
	if (!file) {
		if (errno == ENOENT) {
			return append_empty_logini(filename);
		} else {
			LOGDD("open %s fail, errno=%d", filename, errno);
			return -1;
		}
	}

	while ( fgets(line, sizeof(line)-1, file) != NULL ) {
		if (line[0] == '#') {
			continue;
		}
		if (strncmp(line, LOGD_INI_GROUP, strlen(LOGD_INI_GROUP)) == 0) {
			isValid = 1;
			continue;
		}

		line[sizeof(line)-1] = '\0';
		if ((ptr = get_config_value(line, "log_path")) != NULL) {
			strncpy(g_logdConfig.logPath, ptr, sizeof(g_logdConfig.logPath)-1);
			g_logdConfig.logPath[sizeof(g_logdConfig.logPath)-1] = '\0';
			LOGDD("config: log_path=%s", g_logdConfig.logPath);
			continue;
		}
		if ((ptr = get_config_value(line, "max_log_num")) != NULL) {
			g_logdConfig.maxLogNum = strtol(ptr, NULL, 10);
			LOGDD("config: max_log_num=%d", g_logdConfig.maxLogNum);
			continue;
		}
		if ((ptr = get_config_value(line, "max_log_size")) != NULL) {
			g_logdConfig.maxLogSize = strtol(ptr, NULL, 10);
			LOGDD("config: max_log_size=%d", g_logdConfig.maxLogSize);
			continue;
		}
		if ((ptr = get_config_value(line, "flush_time")) != NULL) {
			g_logdConfig.flushTime = strtol(ptr, NULL, 10);
			LOGDD("config: flush_time=%d", g_logdConfig.flushTime);
			continue;
		}
		if ((ptr = get_config_value(line, "logd_port")) != NULL) {
			g_logdConfig.logdPort = strtol(ptr, NULL, 10);
			LOGDD("config: logd_port=%d", g_logdConfig.logdPort);
			continue;
		}
		if ((ptr = get_config_value(line, "max_msg_size")) != NULL) {
			g_logdConfig.maxMsgSize = strtol(ptr, NULL, 10);
			LOGDD("config: max_msg_size=%d", g_logdConfig.maxMsgSize);
			if (g_buffer) free(g_buffer);
			g_buffer = (char*) malloc(g_logdConfig.maxMsgSize);
			continue;
		}
	}
	fclose(file);

	if (!isValid) {
		LOGDD("%s format is invalid", filename);
		append_empty_logini(filename);
	}
	return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Schedule task function
////////////////////////////////////////////////////////////////////////////////
#ifdef WIN32
double round(double number)
{
    return number < 0.0 ? ceil(number - 0.5) : floor(number + 0.5);
}
#endif

static int64_t get_current_ms_time(void)
{
    int64_t         ms; // Milliseconds
    time_t          s;  // Seconds
    struct timespec spec;

    clock_gettime(CLOCK_REALTIME, &spec);

    s  = spec.tv_sec;
    ms = (int64_t) round(spec.tv_nsec / 1.0e6); // Convert nanoseconds to milliseconds
    if (ms > 999) {
        s++;
        ms = 0;
    }
	return ((int64_t) s) * 1000 + ms;
}

static int trigger_event()
{
	int result;
	unsigned char dummy = 0;
	result = send_udp_message(g_logdStatus.taskClientSocket, &dummy, sizeof(dummy), g_logdStatus.taskServerAddr);
	SOCKET_RESULT_GOTO_ERROR(result, "trigger_event: send_udp_message() failed");

error:
	return (result == 1)? 0: -1;
}

static int consume_event()
{
	unsigned char dummy;
	int bytes = -1;

	while (1) {
		bytes = read_udp_message(g_logdStatus.taskServerSocket, &dummy, sizeof(dummy));
		SOCKET_RESULT_GOTO_ERROR(bytes, "consume_event: read_udp_message() failed");
		if (bytes == 0) { // no data
			break;
		}
	}

	return bytes;

error:
	return -1;
}

static Task* remove_timeout_task(Task** list, int64_t timeoutTime)
{
	Task* task = *list;
	Task* prev = NULL;

	while (task) {
		if (task->timeout <= timeoutTime) { // timeout
			if (prev) {
				prev->next = task->next;
			} else {
				*list = task->next;
			}
			task->next = NULL;
			return task;
		}
		prev = task;
		task = task->next;
	}
	return NULL;
}

/*
	return minum task timeout time, if not found, return 36000
*/
static void min_task_timeout_time(Task* list, struct timeval* tv)
{
	int64_t current = get_current_ms_time();
	int64_t timeout = 36000 + current;
	Task* task = list;

	if (!task) {
		tv->tv_sec = 36000;
		tv->tv_usec = 0;
		return;
	}

	while (task) {
		if (timeout > task->timeout) {
			timeout = task->timeout;
		}
		task = task->next;
	}
	if ((timeout - current) <= 0) {
		tv->tv_sec = 0;
		tv->tv_usec = 0;
	} else {
		tv->tv_sec = (long) (timeout - current) / 1000;
		tv->tv_usec = (long) ((timeout - current) % 1000) * 1000;
	}
}

/*
	Add a new task to task list
	timeout:
		the related time to now in msec.
*/
static int add_task(Task** list, task_act action, void *user_data, int64_t timeout)
{
	Task* task = (Task*) calloc(1, sizeof(Task));
	if (!task) {
		return -1;
	}

	// init task
	task->timeout = get_current_ms_time() + timeout;
	task->action = action;
	task->user_data = user_data;

	// add to list
	if (*list) {
		task->next = *list;
	}
	*list = task;

	// trigger interrupt to re-calculate timeout time
	trigger_event();

	return 0;
}

static void do_task()
{
	int64_t timeoutTime = get_current_ms_time();
	Task* task;

	task = remove_timeout_task(&g_logdStatus.taskList, timeoutTime);

	while (task) {
		if (task->action) {
			task->action(task->user_data);
		}
		free(task); // free, since we have done the task
		task = remove_timeout_task(&g_logdStatus.taskList, timeoutTime);
	}
}

////////////////////////////////////////////////////////////////////////////////
// Logd Function
////////////////////////////////////////////////////////////////////////////////
/*
	flush log file per 5sec. if necessary
*/
static int task_flush_log(void *user_data)
{
	if (g_logdStatus.flushLogSize != g_logdStatus.logSize) { // there are more log in buffer
		g_logdStatus.flushLogSize = g_logdStatus.logSize;
		fflush(g_logdStatus.logFP);
	}
	add_task(&g_logdStatus.taskList, task_flush_log, user_data, g_logdConfig.flushTime*1000);

	return 0;
}

static int uninit_logd()
{
	// close task queue
	if (g_logdStatus.taskClientSocket != INVALID_SOCKET) {
		close(g_logdStatus.taskClientSocket);
	}
	if (g_logdStatus.taskServerSocket != INVALID_SOCKET) {
		close(g_logdStatus.taskServerSocket);
	}

	// close log listener
	if (g_logdStatus.listenSocket != INVALID_SOCKET) {
		close(g_logdStatus.listenSocket);
	}
	// close log file
	if (g_logdStatus.logFP != NULL) {
		fclose(g_logdStatus.logFP);
		g_logdStatus.logFP = NULL;
	}
	
	if (g_buffer) {
		free(g_buffer);
	}
	return 0;
}

static int init_logd()
{
	struct addrinfo *listenAddr;
	int nonblocking = 0;
	char moudlePath[PATH_MAX];
	char filename[PATH_MAX];

	memset(&g_logdConfig, 0, sizeof(g_logdConfig));

	// init log config
	g_logdConfig.maxLogNum = DEF_MAX_LOG_NUM;
	g_logdConfig.maxLogSize = DEF_MAX_LOG_SIZE;
	g_logdConfig.flushTime = 60; // default 60sec.
	g_logdConfig.logdPort = DEF_LOGD_PORT;
	g_logdConfig.maxMsgSize = LOGD_MAX_BUF_LEN;
	util_module_path_get(moudlePath);
	snprintf(g_logdConfig.logPath, sizeof(g_logdConfig.logPath)-1, "%.4069s%c%.4s", moudlePath, FILE_SEPARATOR, DEF_LOG_PATH);

	// load config
	snprintf(filename, sizeof(filename)-1, "%.4086s%c%.7s", moudlePath, FILE_SEPARATOR, "log.ini");
	filename[PATH_MAX-1] = '\0';
	read_log_config(filename);
	
	// carete logs folder
	mkdir_rec(g_logdConfig.logPath, 0777);
	
	// init variable
	g_logdStatus.isRun = 1;
	g_logdStatus.logSize = 0;
	g_buffer = (char*) malloc(g_logdConfig.maxMsgSize);
	
	// init & start log listener
	g_logdStatus.listenSocket = init_server_udp_socket(NULL, g_logdConfig.logdPort, 1, &listenAddr);
	if (g_logdStatus.listenSocket == INVALID_SOCKET) {
		return -1;
	}

	g_logdStatus.taskClientSocket = init_client_udp_socket("127.0.0.1", LOGD_LOCAL_PIPE_PORT, nonblocking, &g_logdStatus.taskServerAddr);
	if (g_logdStatus.taskClientSocket == -1) {
		LOGDD("task_init: init_client_udp_socket() fail");
		goto error;
	}
	
	g_logdStatus.taskServerSocket = init_server_udp_socket("127.0.0.1", LOGD_LOCAL_PIPE_PORT, 1, &listenAddr);
	if (g_logdStatus.taskServerSocket == -1) {
		LOGDD("task_init: init_server_udp_socket() fail");
		goto error;
	}
	
	return 0;

error:
	uninit_logd();

	return -1;
}

static void termination(int sig)
{
	g_logdStatus.isRun = 0;
	trigger_event();
}

static int do_writelog()
{
	char *message = g_buffer + sizeof(int32_t) + sizeof(int32_t); // skip header
	int bytes;
	int32_t length, magic = 0;

	while (1) {
		g_buffer[0] = '\0';
		bytes = read_udp_message(g_logdStatus.listenSocket, (unsigned char*) g_buffer, g_logdConfig.maxMsgSize);
		SOCKET_RESULT_GOTO_ERROR(bytes, "read_udp_message_ex() failed");
		if (bytes == 0) { // no more data
			break;
		}
		if (bytes < sizeof(int32_t) + sizeof(int32_t)) {
			continue; // invalid g_buffer, read next
		}
		memcpy(&magic, g_buffer, sizeof(int32_t));
		if (magic != LOG_MAGIC_NUMBER) {
			continue; // invalid g_buffer, read next
		}
		memcpy(&length, g_buffer + sizeof(int32_t), sizeof(int32_t));
		if ((length + sizeof(int32_t) + sizeof(int32_t)) >= bytes) {
			g_buffer[g_logdConfig.maxMsgSize-1] = '\0';
			// truncated!
		}

		g_logdStatus.logSize += strlen(message) + 1;
		if (g_logdStatus.logSize > g_logdConfig.maxLogSize) {
			log_rotation();
		}
		if (g_logdStatus.logFP) {
			// write log to file
			fputs(message, g_logdStatus.logFP);
			fputc('\n', g_logdStatus.logFP);
		}
	}
	return 0;

error:
	return -1;
}

static int do_socket(fd_set* rfds)
{
	if (g_logdStatus.listenSocket != INVALID_SOCKET && FD_ISSET(g_logdStatus.listenSocket, rfds)) {
		do_writelog();
	} else if (g_logdStatus.taskServerSocket != INVALID_SOCKET && FD_ISSET(g_logdStatus.taskServerSocket, rfds)) { // interrupt, to stop
		consume_event();
		return 0; // receive a interrupt event
	} else { // unknown fd input
		return 0;
	}

	return 0;
}

int main()
{
	fd_set rfds, rfdsCopy;
	static int fdmax = 0;
	struct timeval tv;
	int ret;

	signal(SIGTERM, termination); // end by kill
	signal(SIGINT, termination); // end by ctrl+c
	
	if (init_logd()) {
		return -1;
	}

	// init select
	FD_ZERO(&rfds);
	// set log listen socket
	FD_SET(g_logdStatus.listenSocket, &rfds);
	if ((int)g_logdStatus.listenSocket > fdmax) {
		fdmax = g_logdStatus.listenSocket;
	}
	// set task listen socket
	FD_SET(g_logdStatus.taskServerSocket, &rfds);
	if ((int)g_logdStatus.taskServerSocket > fdmax) {
		fdmax = g_logdStatus.taskServerSocket;
	}
	
	rfdsCopy = rfds;
	tv.tv_sec = 3600;
	tv.tv_usec = 0;

	// add first flush task
	add_task(&g_logdStatus.taskList, task_flush_log, NULL, g_logdConfig.flushTime*1000);
	min_task_timeout_time(g_logdStatus.taskList, &tv);
		
	// do rotation first to get file fp
	log_rotation();
	while (g_logdStatus.isRun) {
		//LOGDD("loop waiting, tv_sec=%ld, tv_usec=%ld..............................", tv.tv_sec, tv.tv_usec);
		ret = select(fdmax+1, &rfds, NULL, NULL, &tv);
		if (ret == -1) {
			LOGDD("select failed: errno=%d", errno);
			break;
		} else if (ret == 0) { // timeout, check task list
			do_task();
		} else { // socket event
			do_socket(&rfds);
		}
		min_task_timeout_time(g_logdStatus.taskList, &tv);
		rfds = rfdsCopy;
	}
	
	uninit_logd();
	LOGDD("end logd");

	return 0;
}
