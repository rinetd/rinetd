#if HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef RETSIGTYPE
#define RETSIGTYPE void
#endif

#ifdef WIN32
#include <windows.h>
#include <winsock.h>
#include "getopt.h"
#define syslog fprintf
#define LOG_ERR stderr
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <getopt.h>
#include <errno.h>
#include <syslog.h>
#define INVALID_SOCKET (-1)
#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#elif HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#endif /* WIN32 */

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <fcntl.h>
#if WIN32 || (!TIME_WITH_SYS_TIME && !HAVE_SYS_TIME_H)
#include <time.h>
#endif
#include <ctype.h>

#ifndef WIN32
/* Windows sockets compatibility defines */
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
int closesocket(int s);

int closesocket(int s) {
	return close(s);
}
#define ioctlsocket ioctl
#define MAKEWORD(a, b)
#define WSAStartup(a, b) (0)
#define	WSACleanup()
#ifdef __MAC__
/* The constants for these are a little screwy in the prelinked
	MSL GUSI lib and we can't rebuild it, so roll with it */
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEAGAIN EAGAIN
#define WSAEINPROGRESS EINPROGRESS
#else
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEAGAIN EAGAIN
#define WSAEINPROGRESS EINPROGRESS
#endif /* __MAC__ */
#define WSAEINTR EINTR
#define SOCKET int
#define GetLastError() (errno)
typedef struct {
	int dummy;
} WSADATA;
#else
/* WIN32 doesn't really have WSAEAGAIN */
#ifndef WSAEAGAIN
#define WSAEAGAIN WSAEWOULDBLOCK
#endif
#endif /* WIN32 */

#ifndef TRUE
#define TRUE 1
#endif

#ifndef FALSE
#define FALSE 0
#endif

#ifdef DEBUG
#define PERROR perror
#else
#define PERROR(x)
#endif /* DEBUG */

/* We've got to get FIONBIO from somewhere. Try the Solaris location
	if it isn't defined yet by the above includes. */
#ifndef FIONBIO
#include <sys/filio.h>
#endif /* FIONBIO */

#include "match.h"

typedef struct _server_info ServerInfo;
struct _server_info
{
	SOCKET fd;

	/* In network order, for network purposes */
	struct in_addr localAddr;
	unsigned short localPort;

	/* In ASCII and local byte order, for logging purposes */
	char *fromHost, *toHost;
	int fromPort, toPort;

	/* Offsets into list of allow and deny rules. Any rules
		prior to globalAllowRules and globalDenyRules are global rules. */
	int allowRules, denyRules;
	int allowRulesTotal, denyRulesTotal;
};

ServerInfo *seInfo = 0;

int globalAllowRules = 0;
int globalDenyRules = 0;

typedef struct _connection_info ConnectionInfo;
struct _connection_info
{
	SOCKET reFd, loFd;
	struct in_addr reAddresses;
	int inputRPos, inputWPos;
	int outputRPos, outputWPos;
	int bytesInput, bytesOutput;
	int coClosed;
	int coClosing;
	int reClosed; // remote closed
	int loClosed; // local closed
	int coLog;
	int server; // only useful for logEvent
	char *input, *output;
};

ConnectionInfo *coInfo = 0;

char **allowRules = 0;
char **denyRules = 0;
int *denyRulesFor = 0;
int seTotal = 0;
int coTotal = 0;
int allowRulesTotal = 0;
int denyRulesTotal = 0;
int maxfd = 0;
char *logFileName = 0;
char *pidLogFileName = 0;
int logFormatCommon = 0;
FILE *logFile = 0;

/* If 'newsize' bytes can be allocated, *data is set to point
	to them, the previous data is copied, and 1 is returned.
	If 'size' bytes cannot be allocated, *data is UNCHANGED,
	and 0 is returned. */

#define SAFE_REALLOC(x, y, z) safeRealloc((void **) (x), (y), (z))

int safeRealloc(void **data, int oldsize, int newsize);

/*
	se: (se)rver sockets
	re: (re)mote sockets
	lo: (lo)cal sockets (being redirected to)
	co: connections
*/

#define bufferSpace 1024

void readConfiguration();

/* Signal handlers */
RETSIGTYPE plumber(int s);
RETSIGTYPE hup(int s);
RETSIGTYPE term(int s);

void initArrays(void);
void RegisterPID(void);

void selectLoop(void);

void logEvent(ConnectionInfo const *c, int i, int result);

int getAddress(char *host, struct in_addr *iaddr);

char const *logMessages[] = {
	"done-local-closed",
	"done-remote-closed",
	"accept-failed -",
	0,
	"local-socket-failed -",
	0,
	"local-bind-failed -",
	0,
	"local-connect-failed -",
	0,
	"opened",
	0,
	"not-allowed",
	0,
	"denied",
	0,
};

enum
{
	logDone = 0,
	logAcceptFailed = 2,
	logLocalSocketFailed = 4,
	logLocalBindFailed = 6,
	logLocalConnectFailed = 8,
	logOpened = 10,
	logNotAllowed = 12,
	logDenied = 14,

	logLocalClosedFirst = 0,
	logRemoteClosedFirst = 1,
};

/* Option parsing */

typedef struct _rinetd_options RinetdOptions;
struct _rinetd_options
{
	char const *conf_file;
	int foreground;
};

RinetdOptions options = {
	"/etc/rinetd.conf",
	0,
};

int readArgs (int argc,
	char **argv,
	RinetdOptions *options);

int main(int argc, char *argv[])
{
#ifdef WIN32
	WSADATA wsaData;
#endif
	int result;
#ifndef WIN32
	openlog("rinetd", LOG_PID, LOG_DAEMON);
#endif
	result = WSAStartup(MAKEWORD(1, 1), &wsaData);
	if (result != 0) {
		fprintf(stderr, "Your computer was not connected "
			"to the Internet at the time that "
			"this program was launched, or you "
			"do not have a 32-bit "
			"connection to the Internet.");
		exit(1);
	}
	readArgs(argc, argv, &options);
#ifndef WIN32
#ifdef DEBUG
	{
#elif HAVE_DAEMON
	if (options.foreground || !daemon(0, 0)) {
#else
	if (options.foreground || !fork()) {
#endif
#ifdef HAVE_SIGACTION
			struct sigaction act;
			act.sa_handler=SIG_IGN;
			sigemptyset (&act.sa_mask);
			act.sa_flags=SA_RESTART;
			sigaction(SIGPIPE, &act, NULL);
			act.sa_handler=&hup;
			sigaction(SIGHUP, &act, NULL);
#else
			signal(SIGPIPE, plumber);
			signal(SIGHUP, hup);
#endif
#endif /* WIN32 */
			signal(SIGTERM, term);
			initArrays();
			readConfiguration();
			RegisterPID();
			syslog(LOG_INFO, "Starting redirections...");
			selectLoop();
#ifndef WIN32
#ifndef DEBUG
	} else {
		exit(0);
#endif
	}
#endif /* WIN32 */
	return 0;
}

int getConfLine(FILE *in, char *line, int space, int *lnum);

int patternBad(char *pattern);

void readConfiguration(void)
{
	FILE *in;
	char line[16384];
	int lnum = 0;
	int i;
	int ai;
	int di;
	if (seInfo) {
		/* Close existing server sockets. */
		for (i = 0; i < seTotal; i++) {
			ServerInfo *s = &seInfo[i];
			if (s->fd != -1) {
				closesocket(s->fd);
				free(s->fromHost);
				free(s->toHost);
			}
		}
		/* Free memory associated with previous set. */
		free(seInfo);
	}
	seTotal = 0;
	if (allowRules) {
		/* Forget existing allow rules. */
		for (i = 0; (i < allowRulesTotal); i++) {
			free(allowRules[i]);
		}
		/* Free memory associated with previous set. */
		free(allowRules);
		globalAllowRules = 0;
	}
	allowRulesTotal = 0;
	if (denyRules) {
		/* Forget existing deny rules. */
		for (i = 0; (i < denyRulesTotal); i++) {
			free(denyRules[i]);
		}
		/* Free memory associated with previous set. */
		free(denyRules);
		globalDenyRules = 0;
	}
	denyRulesTotal = 0;
	if (logFileName) {
		free(logFileName);
		logFileName = 0;
	}
	if (pidLogFileName) {
		free(pidLogFileName);
		pidLogFileName = 0;
	}
	/* 1. Count the non-comment lines of each type and
		allocate space for the data. */
	in = fopen(options.conf_file, "r");
	if (!in) {
		fprintf(stderr, "rinetd: can't open %s\n", options.conf_file);
		exit(1);
	}
	while (1) {
		char *t = 0;
		if (!getConfLine(in, line, sizeof(line), &lnum)) {
			break;
		}
		t = strtok(line, " \t\r\n");
		if (!strcmp(t, "logfile")) {
			continue;
		} else if (!strcmp(t, "pidlogfile")) {
			continue;
		} else if (!strcmp(t, "logcommon")) {
			continue;
		} else if (!strcmp(t, "allow")) {
			allowRulesTotal++;
		} else if (!strcmp(t, "deny")) {
			denyRulesTotal++;
		} else {
			/* A regular forwarding rule */
			seTotal++;
		}
	}
	fclose(in);
	seInfo = (ServerInfo *) malloc(sizeof(ServerInfo) * seTotal);
	if (!seInfo) {
		goto lowMemory;
	}
	for (i = 0; i < seTotal; ++i) {
		memset(&seInfo[i], 0, sizeof(seInfo[i]));
		seInfo[i].fd = INVALID_SOCKET;
	}
	allowRules = (char **)
		malloc(sizeof(char *) * allowRulesTotal);
	if (!allowRules) {
		goto lowMemory;
	}
	denyRules = (char **)
		malloc(sizeof(char *) * denyRulesTotal);
	if (!denyRules) {
		goto lowMemory;
	}
	/* 2. Make a second pass to configure them. */
	i = 0;
	ai = 0;
	di = 0;
	lnum = 0;
	in = fopen(options.conf_file, "r");
	if (!in) {
		goto lowMemory;
	}
	while (1) {
		char *bindAddress;
		unsigned short bindPort;
		char *connectAddress;
		char *bindPortS;
		char *connectPortS;
		unsigned short connectPort;
		struct in_addr iaddr;
		struct sockaddr_in saddr;
		struct servent *service;
		int j;
		if (!getConfLine(in, line, sizeof(line), &lnum)) {
			break;
		}
		bindAddress = strtok(line, " \t\r\n");
		if (!bindAddress) {
			syslog(LOG_ERR, "no bind address specified "
				"on file %s, line %d.\n", options.conf_file, lnum);
			continue;
		}
		if (!strcmp(bindAddress, "allow")) {
			char *pattern = strtok(0, " \t\r\n");
			if (!pattern) {
				syslog(LOG_ERR, "nothing to allow "
					"specified on file %s, line %d.\n", options.conf_file, lnum);
				continue;
			}
			if (patternBad(pattern)) {
				syslog(LOG_ERR, "illegal allow or "
					"deny pattern. Only digits, ., and\n"
					"the ? and * wild cards are allowed. "
					"For performance reasons, rinetd\n"
					"does not look up complete "
					"host names.\n");
				continue;
			}

			allowRules[ai] = strdup(pattern);
			if (!allowRules[ai]) {
				goto lowMemory;
			}
			if (i > 0) {
				if (seInfo[i - 1].allowRulesTotal == 0) {
					seInfo[i - 1].allowRules = ai;
				}
				seInfo[i - 1].allowRulesTotal++;
			} else {
				globalAllowRules++;
			}
			ai++;
		} else if (!strcmp(bindAddress, "deny")) {
			char *pattern = strtok(0, " \t\r\n");
			if (!pattern) {
				syslog(LOG_ERR, "nothing to deny "
					"specified on file %s, line %d.\n", options.conf_file, lnum);
				continue;
			}
			denyRules[di] = strdup(pattern);
			if (!denyRules[di]) {
				goto lowMemory;
			}
			if (i > 0) {
				if (seInfo[i - 1].denyRulesTotal == 0) {
					seInfo[i - 1].denyRules = di;
				}
				seInfo[i - 1].denyRulesTotal++;
			} else {
				globalDenyRules++;
			}
			di++;
		} else if (!strcmp(bindAddress, "logfile")) {
			char *nt = strtok(0, " \t\r\n");
			if (!nt) {
				syslog(LOG_ERR, "no log file name "
					"specified on file %s, line %d.\n", options.conf_file, lnum);
				continue;
			}
			logFileName = strdup(nt);
			if (!logFileName) {
				goto lowMemory;
			}
		} else if (!strcmp(bindAddress, "pidlogfile")) {
			char *nt = strtok(0, " \t\r\n");
			if (!nt) {
				syslog(LOG_ERR, "no PID log file name "
					"specified on file %s, line %d.\n", options.conf_file, lnum);
				continue;
			}
			pidLogFileName = strdup(nt);
			if (!pidLogFileName) {
				goto lowMemory;
			}
		} else if (!strcmp(bindAddress, "logcommon")) {
			logFormatCommon = 1;
		} else {
			/* A regular forwarding rule. */
			ServerInfo *s = &seInfo[i];
			bindPortS = strtok(0, " \t\r\n");
			if (!bindPortS) {
				syslog(LOG_ERR, "no bind port "
					"specified on file %s, line %d.\n", options.conf_file, lnum);
				continue;
			}
			service = getservbyname(bindPortS, "tcp");
			if (service) {
				bindPort = ntohs(service->s_port);
			} else {
				bindPort = atoi(bindPortS);
			}
			if ((bindPort == 0) || (bindPort >= 65536)) {
				syslog(LOG_ERR, "bind port missing "
					"or out of range on file %s, line %d.\n", options.conf_file, lnum);
				continue;
			}
			connectAddress = strtok(0, " \t\r\n");
			if (!connectAddress) {
				syslog(LOG_ERR, "no connect address "
					"specified on file %s, line %d.\n", options.conf_file, lnum);
				continue;
			}
			connectPortS = strtok(0, " \t\r\n");
			if (!connectPortS) {
				syslog(LOG_ERR, "no connect port "
					"specified on file %s, line %d.\n", options.conf_file, lnum);
				continue;
			}
			service = getservbyname(connectPortS, "tcp");
			if (service) {
				connectPort = ntohs(service->s_port);
			} else {
				connectPort = atoi(connectPortS);
			}
			if ((connectPort == 0) || (connectPort >= 65536)) {
				syslog(LOG_ERR, "bind port missing "
					"or out of range on file %s,  %d.\n", options.conf_file, lnum);
				continue;
			}
			/* Turn all of this stuff into reasonable addresses */
			if (!getAddress(bindAddress, &iaddr)) {
				fprintf(stderr, "rinetd: host %s could not be "
					"resolved on line %d.\n",
					bindAddress, lnum);
				continue;
			}
			/* Make a server socket */
			s->fd = socket(PF_INET, SOCK_STREAM, 0);
			if (s->fd == INVALID_SOCKET) {
				syslog(LOG_ERR, "couldn't create "
					"server socket! (%m)\n");
				s->fd = -1;
				continue;
			}
#ifndef WIN32
			if (s->fd > maxfd) {
				maxfd = s->fd;
			}
#endif
			saddr.sin_family = AF_INET;
			memcpy(&saddr.sin_addr, &iaddr, sizeof(iaddr));
			saddr.sin_port = htons(bindPort);
			j = 1;
			setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR,
				(const char *) &j, sizeof(j));
			if (bind(s->fd, (struct sockaddr *)
				&saddr, sizeof(saddr)) == SOCKET_ERROR)
			{
				/* Warn -- don't exit. */
				syslog(LOG_ERR, "couldn't bind to "
					"address %s port %d (%m)\n",
					bindAddress, bindPort);
				closesocket(s->fd);
				s->fd = INVALID_SOCKET;
				continue;
			}
			if (listen(s->fd, 5) == SOCKET_ERROR) {
				/* Warn -- don't exit. */
				syslog(LOG_ERR, "couldn't listen to "
					"address %s port %d (%m)\n",
					bindAddress, bindPort);
				closesocket(s->fd);
				s->fd = INVALID_SOCKET;
				continue;
			}
			ioctlsocket(s->fd, FIONBIO, &j);
			if (!getAddress(connectAddress, &iaddr)) {
				/* Warn -- don't exit. */
				syslog(LOG_ERR, "host %s could not be "
					"resolved on file %s, line %d.\n",
					bindAddress, options.conf_file, lnum);
				closesocket(s->fd);
				s->fd = INVALID_SOCKET;
				continue;
			}
			s->localAddr = iaddr;
			s->localPort = htons(connectPort);
			s->fromHost = strdup(bindAddress);
			if (!s->fromHost) {
				goto lowMemory;
			}
			s->fromPort = bindPort;
			s->toHost = strdup(connectAddress);
			if (!s->toHost) {
				goto lowMemory;
			}
			s->toPort = connectPort;
			i++;
		}
	}
	fclose(in);
	/* Open the log file */
	if (logFile) {
		fclose(logFile);
		logFile = 0;
	}
	if (logFileName) {
		logFile = fopen(logFileName, "a");
		if (logFile) {
			setvbuf(logFile, NULL, _IONBF, 0);
		} else {
			syslog(LOG_ERR, "could not open %s to append (%m).\n",
				logFileName);
		}
	}
	return;
lowMemory:
	syslog(LOG_ERR, "not enough memory to start rinetd.\n");
	exit(1);
}

int getConfLine(FILE *in, char *line, int space, int *lnum)
{
	char *p;
	while (1) {
		(*lnum)++;
		if (!fgets(line, space, in)) {
			return 0;
		}
		p = line;
		while (isspace(*p)) {
			p++;
		}
		if (!(*p)) {
			/* Blank lines are OK */
			continue;
		}
		if (*p == '#') {
			/* Comment lines are also OK */
			continue;
		}
		return 1;
	}
}

void initArrays(void)
{
	int j;
	coTotal = 64;
	coInfo = (ConnectionInfo *) malloc(sizeof(ConnectionInfo) * coTotal);
	if (!coInfo) {
		syslog(LOG_ERR, "not enough memory to start rinetd.\n");
		exit(1);
	}
	for (j = 0; j < coTotal; j++) {
		ConnectionInfo *c = &coInfo[j];
		memset(c, 0, sizeof(*c));
		c->coClosed = 1;
		c->input = (char *) malloc(sizeof(char) * bufferSpace);
		c->output = (char *) malloc(sizeof(char) * bufferSpace);
		if (!c->input || !c->output) {
			syslog(LOG_ERR, "not enough memory to start rinetd.\n");
			exit(1);
		}
	}
}

void selectPass(void);

void selectLoop(void) {
	while (1) {
		selectPass();
	}
}

void handleRemoteWrite(ConnectionInfo *c);
void handleRemoteRead(ConnectionInfo *c);
void handleLocalWrite(ConnectionInfo *c);
void handleLocalRead(ConnectionInfo *c);
void handleCloseFromLocal(ConnectionInfo *c);
void handleCloseFromRemote(ConnectionInfo *c);
void handleAccept(int i);
void openLocalFd(int se, ConnectionInfo *c);
int getAddress(char *host, struct in_addr *iaddr);

void selectPass(void) {
	int i;
	fd_set readfds, writefds;
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	/* Server sockets */
	for (i = 0; (i < seTotal); i++) {
		if (seInfo[i].fd != INVALID_SOCKET) {
			FD_SET(seInfo[i].fd, &readfds);
		}
	}
	/* Connection sockets */
	for (i = 0; (i < coTotal); i++) {
		ConnectionInfo *c = &coInfo[i];
		if (c->coClosed) {
			continue;
		}
		if (c->coClosing) {
			if (!c->reClosed) {
				FD_SET(c->reFd, &writefds);
			}
			if (!c->loClosed) {
				FD_SET(c->loFd, &writefds);
			}
		}
		/* Get more input if we have room for it */
		if ((!c->reClosed) && (c->inputRPos < bufferSpace)) {
			FD_SET(c->reFd, &readfds);
		}
		/* Send more output if we have any */
		if ((!c->reClosed) && (c->outputWPos < c->outputRPos)) {
			FD_SET(c->reFd, &writefds);
		}
		/* Accept more output from the local
			server if there's room */
		if ((!c->loClosed) && (c->outputRPos < bufferSpace)) {
			FD_SET(c->loFd, &readfds);
		}
		/* Send more input to the local server
			if we have any */
		if ((!c->loClosed) && (c->inputWPos < c->inputRPos)) {
			FD_SET(c->loFd, &writefds);
		}
	}
	select(maxfd + 1, &readfds, &writefds, 0, 0);
	for (i = 0; (i < seTotal); i++) {
		if (seInfo[i].fd != -1) {
			if (FD_ISSET(seInfo[i].fd, &readfds)) {
				handleAccept(i);
			}
		}
	}
	for (i = 0; (i < coTotal); i++) {
		ConnectionInfo *c = &coInfo[i];
		if (c->coClosed) {
			continue;
		}
		if (!c->reClosed) {
			if (FD_ISSET(c->reFd, &readfds)) {
				handleRemoteRead(c);
			}
		}
		if (!c->reClosed) {
			if (FD_ISSET(c->reFd, &writefds)) {
				handleRemoteWrite(c);
			}
		}
		if (!c->loClosed) {
			if (FD_ISSET(c->loFd, &readfds)) {
				handleLocalRead(c);
			}
		}
		if (!c->loClosed) {
			if (FD_ISSET(c->loFd, &writefds)) {
				handleLocalWrite(c);
			}
		}
		if (c->loClosed && c->reClosed) {
			c->coClosed = 1;
		}
	}
}

void handleRemoteRead(ConnectionInfo *c)
{
	int got;
	if (bufferSpace == c->inputRPos) {
		return;
	}
	got = recv(c->reFd, c->input + c->inputRPos,
		bufferSpace - c->inputRPos, 0);
	if (got == 0) {
		/* Prepare for closing */
		handleCloseFromRemote(c);
		return;
	}
	if (got < 0) {
		if (GetLastError() == WSAEWOULDBLOCK) {
			return;
		}
		if (GetLastError() == WSAEINPROGRESS) {
			return;
		}
		handleCloseFromRemote(c);
		return;
	}
	c->bytesInput += got;
	c->inputRPos += got;
}

void handleRemoteWrite(ConnectionInfo *c)
{
	int got;
	if (c->coClosing && (c->outputWPos == c->outputRPos)) {
		c->reClosed = 1;
		c->coClosed = 1;
		PERROR("rinetd: local closed and no more output");
		logEvent(c, c->server, logDone | c->coLog);
		closesocket(c->reFd);
		return;
	}
	got = send(c->reFd, c->output + c->outputWPos,
		c->outputRPos - c->outputWPos, 0);
	if (got < 0) {
		if (GetLastError() == WSAEWOULDBLOCK) {
			return;
		}
		if (GetLastError() == WSAEINPROGRESS) {
			return;
		}
		handleCloseFromRemote(c);
		return;
	}
	c->outputWPos += got;
	if (c->outputWPos == c->outputRPos) {
		c->outputWPos = 0;
		c->outputRPos = 0;
	}
	c->bytesOutput += got;
}

void handleLocalRead(ConnectionInfo *c)
{
	int got;
	if (bufferSpace == c->outputRPos) {
		return;
	}
	got = recv(c->loFd, c->output + c->outputRPos,
		bufferSpace - c->outputRPos, 0);
	if (got == 0) {
		handleCloseFromLocal(c);
		return;
	}
	if (got < 0) {
		if (GetLastError() == WSAEWOULDBLOCK) {
			return;
		}
		if (GetLastError() == WSAEINPROGRESS) {
			return;
		}
		handleCloseFromLocal(c);
		return;
	}
	c->outputRPos += got;
}

void handleLocalWrite(ConnectionInfo *c)
{
	int got;
	if (c->coClosing && (c->inputWPos == c->inputRPos)) {
		c->loClosed = 1;
		c->coClosed = 1;
		PERROR("remote closed and no more input");
		logEvent(c, c->server, logDone | c->coLog);
		closesocket(c->loFd);
		return;
	}
	got = send(c->loFd, c->input + c->inputWPos,
		c->inputRPos - c->inputWPos, 0);
	if (got < 0) {
		if (GetLastError() == WSAEWOULDBLOCK) {
			return;
		}
		if (GetLastError() == WSAEINPROGRESS) {
			return;
		}
		handleCloseFromLocal(c);
		return;
	}
	c->inputWPos += got;
	if (c->inputWPos == c->inputRPos) {
		c->inputWPos = 0;
		c->inputRPos = 0;
	}
}

void handleCloseFromLocal(ConnectionInfo *c)
{
	c->coClosing = 1;
	/* The local end fizzled out, so make sure
		we're all done with that */
	PERROR("close from local");
	closesocket(c->loFd);
	c->loClosed = 1;
	if (!c->reClosed) {
#ifndef __linux__
#ifndef WIN32
		/* Now set up the remote end for a polite closing */

		/* Request a low-water mark equal to the entire
			output buffer, so the next write notification
			tells us for sure that we can close the socket. */
		int arg = 1024;
		setsockopt(c->reFd, SOL_SOCKET, SO_SNDLOWAT,
			&arg, sizeof(arg));
#endif /* WIN32 */
#endif /* __linux__ */
		c->coLog = logLocalClosedFirst;
	}
}

void handleCloseFromRemote(ConnectionInfo *c)
{
	c->coClosing = 1;
	/* The remote end fizzled out, so make sure
		we're all done with that */
	PERROR("close from remote");
	closesocket(c->reFd);
	c->reClosed = 1;
	if (!c->loClosed) {
#ifndef __linux__
#ifndef WIN32
		/* Now set up the local end for a polite closing */

		/* Request a low-water mark equal to the entire
			output buffer, so the next write notification
			tells us for sure that we can close the socket. */
		int arg = 1024;
		setsockopt(c->loFd, SOL_SOCKET, SO_SNDLOWAT,
			&arg, sizeof(arg));
#endif /* WIN32 */
#endif /* __linux__ */
		c->loClosed = 0;
		c->coLog = logRemoteClosedFirst;
	}
}

void refuse(ConnectionInfo *c, int logCode);

void handleAccept(int i)
{
	ServerInfo *s = &seInfo[i];
	ConnectionInfo *c = NULL;
	struct sockaddr addr;
	struct sockaddr_in *sin;
	struct in_addr address;
	char const *addressText;
	int j;
#if HAVE_SOCKLEN_T
	socklen_t addrlen;
#else
	int addrlen;
#endif
	int o;
	SOCKET nfd;
	addrlen = sizeof(addr);
	nfd = accept(s->fd, &addr, &addrlen);
	if (nfd == INVALID_SOCKET) {
		syslog(LOG_ERR, "accept(%d): %m", s->fd);
		logEvent(NULL, i, logAcceptFailed);
		return;
	}
#ifndef WIN32
	if (nfd > maxfd) {
		maxfd = nfd;
	}
#endif /* WIN32 */
	j = 1;
	ioctlsocket(nfd, FIONBIO, &j);
	j = 0;
#ifndef WIN32
	setsockopt(nfd, SOL_SOCKET, SO_LINGER, &j, sizeof(j));
#endif
	for (j = 0; (j < coTotal); j++) {
		if (coInfo[j].coClosed) {
			c = &coInfo[j];
			break;
		}
	}
	if (c == NULL) {
		o = coTotal;
		coTotal *= 2;
		if (!SAFE_REALLOC(&coInfo, sizeof(ConnectionInfo) * o,
			sizeof(ConnectionInfo) * coTotal))
		{
			goto shortage;
		}
		for (j = o; j < coTotal; j++) {
			memset(&coInfo[j], 0, sizeof(coInfo[j]));
			coInfo[j].coClosed = 1;
			coInfo[j].input = (char *)
				malloc(sizeof(char) * bufferSpace);
			if (!coInfo[j].input) {
				int k;
				for (k = o; (k < j); k++) {
					free(coInfo[k].input);
					free(coInfo[k].output);
				}
				goto shortage;
			}
			coInfo[j].output = (char *)
				malloc(sizeof(char) * bufferSpace);
			if (!coInfo[j].output) {
				int k;
				free(coInfo[j].input);
				for (k = o; (k < j); k++) {
					free(coInfo[k].input);
					free(coInfo[k].output);
				}
				goto shortage;
			}
		}
		c = &coInfo[o];
	}
	c->inputRPos = 0;
	c->inputWPos = 0;
	c->outputRPos = 0;
	c->outputWPos = 0;
	c->coClosed = 0;
	c->coClosing = 0;
	c->reClosed = 0;
	c->loClosed = 0;
	c->reFd = nfd;
	c->bytesInput = 0;
	c->bytesOutput = 0;
	c->coLog = 0;
	c->server = i;
	sin = (struct sockaddr_in *) &addr;
	c->reAddresses.s_addr = address.s_addr = sin->sin_addr.s_addr;
	addressText = inet_ntoa(address);
	/* 1. Check global allow rules. If there are no
		global allow rules, it's presumed OK at
		this step. If there are any, and it doesn't
		match at least one, kick it out. */
	if (globalAllowRules) {
		int good = 0;
		for (j = 0; (j < globalAllowRules); j++) {
			if (match(addressText, allowRules[j])) {
				good = 1;
				break;
			}
		}
		if (!good) {
			refuse(c, logNotAllowed);
			return;
		}
	}
	/* 2. Check global deny rules. If it matches
		any of the global deny rules, kick it out. */
	if (globalDenyRules) {
		for (j = 0; (j < globalDenyRules); j++) {
			if (match(addressText, denyRules[j])) {
				refuse(c, logDenied);
			}
		}
	}
	/* 3. Check allow rules specific to this forwarding rule.
		If there are none, it's OK. If there are any,
		it must match at least one. */
	if (s->allowRulesTotal) {
		int good = 0;
		for (j = 0; (j < s->allowRulesTotal); j++) {
			if (match(addressText,
				allowRules[s->allowRules + j])) {
				good = 1;
				break;
			}
		}
		if (!good) {
			refuse(c, logNotAllowed);
			return;
		}
	}
	/* 2. Check deny rules specific to this forwarding rule. If
		it matches any of the deny rules, kick it out. */
	if (s->denyRulesTotal) {
		for (j = 0; (j < s->denyRulesTotal); j++) {
			if (match(addressText,
				denyRules[s->denyRules + j])) {
				refuse(c, logDenied);
			}
		}
	}
	/* Now open a connection to the local server.
		This, too, is nonblocking. Why wait
		for anything when you don't have to? */
	openLocalFd(i, c);
	return;
shortage:
	syslog(LOG_ERR, "not enough memory to add slots. "
		"Currently %d slots.\n", o);
	/* Go back to the previous total number of slots */
	coTotal = o;
}

void openLocalFd(int se, ConnectionInfo *c)
{
	int j;
	struct sockaddr_in saddr;
	c->loFd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (c->loFd == INVALID_SOCKET) {
		syslog(LOG_ERR, "socket(): %m");
		closesocket(c->reFd);
		c->reClosed = 1;
		c->loClosed = 1;
		c->coClosed = 1;
		logEvent(c, c->server, logLocalSocketFailed);
		return;
	}
#ifndef WIN32
	if (c->loFd > maxfd) {
		maxfd = c->loFd;
	}
#endif /* WIN32 */

#if 0 // You don't need bind(2) on a socket you'll use for connect(2).
	/* Bind the local socket */
	saddr.sin_family = AF_INET;
	saddr.sin_port = INADDR_ANY;
	saddr.sin_addr.s_addr = 0;
	if (bind(c->loFd, (struct sockaddr *) &saddr, sizeof(saddr)) == SOCKET_ERROR) {
		closesocket(c->loFd);
		closesocket(c->reFd);
		c->reClosed = 1;
		c->loClosed = 1;
		c->coClosed = 1;
		logEvent(c, c->server, logLocalBindFailed);
		return;
	}
#endif

	memset(&saddr, 0, sizeof(struct sockaddr_in));
	saddr.sin_family = AF_INET;
	memcpy(&saddr.sin_addr, &seInfo[se].localAddr, sizeof(struct in_addr));
	saddr.sin_port = seInfo[se].localPort;
#ifndef WIN32
#ifdef __linux__
	j = 0;
	setsockopt(c->loFd, SOL_SOCKET, SO_LINGER, &j, sizeof(j));
#else
	j = 1024;
	setsockopt(c->loFd, SOL_SOCKET, SO_SNDBUF, &j, sizeof(j));
#endif /* __linux__ */
#endif /* WIN32 */
	j = 1;
	ioctlsocket(c->loFd, FIONBIO, &j);
	if (connect(c->loFd, (struct sockaddr *)&saddr,
		sizeof(struct sockaddr_in)) == INVALID_SOCKET)
	{
		if ((GetLastError() != WSAEINPROGRESS) &&
			(GetLastError() != WSAEWOULDBLOCK))
		{
			PERROR("rinetd: connect");
			closesocket(c->loFd);
			closesocket(c->reFd);
			c->reClosed = 1;
			c->loClosed = 1;
			c->coClosed = 1;
			logEvent(c, c->server, logLocalConnectFailed);
			return;
		}
	}
	logEvent(c, c->server, logOpened);
}

int getAddress(char *host, struct in_addr *iaddr)
{
	char *p = host;
	int ishost = 0;
	while (*p) {
		if (!isdigit(*p) && (*p) != '.') {
			ishost = 1;
			break;
		}
		p++;
	}
	if (ishost) {
		struct hostent *h;
		h = gethostbyname(host);
		if (!h) {
			const char *msg = "(unknown DNS error)";
			switch(h_errno)
			{
			case HOST_NOT_FOUND:
				msg = "The specified host is unknown.";
				break;
			case NO_ADDRESS:
				msg = "The requested name is valid but does not have an IP address.";
				break;
			case NO_RECOVERY:
				msg = "A non-recoverable name server error occurred.";
				break;
			case TRY_AGAIN:
				msg = "A temporary error occurred on an authoritative name server.  Try again later.";
				break;
			}
			syslog(LOG_ERR, "While resolving `%s' got: %s", host, msg);
			return 0;
		}
		memcpy(
			(void *) &iaddr->s_addr,
			(void *) h->h_addr,
			4);
		return 1;
	} else {
		iaddr->s_addr = inet_addr(host);
		return 1;
	}
}

#ifndef WIN32
#ifndef HAVE_SIGACTION
RETSIGTYPE plumber(int s)
{
	/* Just reinstall */
	signal(SIGPIPE, plumber);
}
#endif

RETSIGTYPE hup(int s)
{
	syslog(LOG_INFO, "Received SIGHUP, reloading configuration...");
	/* Learn the new rules */
	readConfiguration();
#ifndef HAVE_SIGACTION
	/* And reinstall the signal handler */
	signal(SIGHUP, hup);
#endif
}
#endif /* WIN32 */

int safeRealloc(void **data, int oldsize, int newsize)
{
	void *newData = malloc(newsize + 1);
	if (!newData) {
		return 0;
	}
	if (newsize < oldsize) {
		memcpy(newData, *data, newsize);
	} else {
		memcpy(newData, *data, oldsize);
	}
	*data = newData;
	return 1;
}

void RegisterPID(void)
{
	FILE *pid_file;
	char const *pid_file_name = "/var/run/rinetd.pid";
	if (pidLogFileName) {
		pid_file_name = pidLogFileName;
	}
/* add other systems with wherever they register processes */
#if	defined(__linux__)
	pid_file = fopen(pid_file_name, "w");
	if (pid_file == NULL) {
		/* non-fatal, non-Linux may lack /var/run... */
		fprintf(stderr, "rinetd: Couldn't write to "
			"%s. PID was not logged.\n", pid_file_name);
		goto error;
	} else {
		fprintf(pid_file, "%d\n", getpid());
		/* errors aren't fatal */
		if(fclose(pid_file))
			goto error;
	}
	return;
error:
	syslog(LOG_ERR, "Couldn't write to "
		"%s. PID was not logged (%m).\n", pid_file_name);
#endif	/* __linux__ */
}

struct in_addr nullAddress = { 0 };

struct tm *get_gmtoff(int *tz);

void logEvent(ConnectionInfo const *c, int i, int result)
{
	ServerInfo const *s = &seInfo[i];
	struct in_addr const *reAddress;
	char const *addressText;
	int bytesOutput;
	int bytesInput;
	/* Bit of borrowing from Apache logging module here,
		thanks folks */
	int timz;
	struct tm *t;
	char tstr[1024];
	char sign;
	t = get_gmtoff(&timz);
	sign = (timz < 0 ? '-' : '+');
	if (timz < 0) {
		timz = -timz;
	}
	strftime(tstr, sizeof(tstr), "%d/%b/%Y:%H:%M:%S ", t);

	if (c != NULL) {
		reAddress = &c->reAddresses;
		bytesOutput = c->bytesOutput;
		bytesInput = c->bytesInput;
	} else {
		reAddress = &nullAddress;
		bytesOutput = 0;
		bytesInput = 0;
	}
	addressText = inet_ntoa(*reAddress);
	if(result==logNotAllowed || result==logDenied)
		syslog(LOG_INFO,"%s %s"
			,addressText
			,logMessages[result]);
	if (logFile) {
		if (logFormatCommon) {
			/* Fake a common log format log file in a way that
				most web analyzers can do something interesting with.
				We lie and say the protocol is HTTP because we don't
				want the web analyzer to reject the line. We also
				lie and claim success (code 200) because we don't
				want the web analyzer to ignore the line as an
				error and not analyze the "URL." We put a result
				message into our "URL" instead. The last field
				is an extra, giving the number of input bytes,
				after several placeholders meant to fill the
				positions frequently occupied by user agent,
				referrer, and server name information. */
			fprintf(logFile, "%s - - "
				"[%s %c%.2d%.2d] "
				"\"GET /rinetd-services/%s/%d/%s/%d/%s HTTP/1.0\" "
				"200 %d - - - %d\n",
				addressText,
				tstr,
				sign,
				timz / 60,
				timz % 60,
				s->fromHost, s->fromPort,
				s->toHost, s->toPort,
				logMessages[result],
				bytesOutput,
				bytesInput);
		} else {
			/* Write an rinetd-specific log entry with a
				less goofy format. */
			fprintf(logFile, "%s\t%s\t%s\t%d\t%s\t%d\t%d"
					"\t%d\t%s\n",
				tstr,
				addressText,
				s->fromHost, s->fromPort,
				s->toHost, s->toPort,
				bytesInput,
				bytesOutput,
				logMessages[result]);
		}
	}
}

int readArgs (int argc,
	char **argv,
	RinetdOptions *options)
{
	int c;

	while (1) {
		int option_index = 0;
		static struct option long_options[] = {
			{"conf-file",  1, 0, 'c'},
			{"foreground", 0, 0, 'f'},
			{"help",       0, 0, 'h'},
			{"version",    0, 0, 'v'},
			{0, 0, 0, 0}
		};
		c = getopt_long (argc, argv, "c:fshv",
			long_options, &option_index);
		if (c == -1) {
			break;
		}
		switch (c) {
			case 'c':
			options->conf_file = strdup(optarg);
			if (!options->conf_file) {
				syslog(LOG_ERR, "Not enough memory to "
					"launch rinetd.\n");
				exit(1);
			}
			break;
			case 'f':
			options->foreground=1;
			break;
			case 'h':
			printf("Usage: rinetd [OPTION]\n"
				"  -c, --conf-file FILE   read configuration "
				"from FILE\n"
				"  -f, --foreground       do not run in the "
				"background\n"
				"  -h, --help             display this help\n"
				"  -v, --version          display version "
				"number\n\n");
			printf("Most options are controlled through the\n"
				"configuration file. See the rinetd(8)\n"
				"manpage for more information.\n");
			exit (0);
			case 'v':
			printf ("rinetd %s\n", PACKAGE_VERSION);
			exit (0);
			case '?':
			default:
			exit (1);
		}
	}
	return 0;
}

/* get_gmtoff was borrowed from Apache. Thanks folks. */

struct tm *get_gmtoff(int *tz) {
	time_t tt = time(NULL);
	struct tm gmt;
	struct tm *t;
	int days, hours, minutes;

	/* Assume we are never more than 24 hours away. */
	gmt = *gmtime(&tt); /* remember gmtime/localtime return ptr to static */
	t = localtime(&tt); /* buffer... so be careful */
	days = t->tm_yday - gmt.tm_yday;
	hours = ((days < -1 ? 24 : 1 < days ? -24 : days * 24)
		+ t->tm_hour - gmt.tm_hour);
	minutes = hours * 60 + t->tm_min - gmt.tm_min;
	*tz = minutes;
	return t;
}

int patternBad(char *pattern)
{
	char *p = pattern;
	while (*p) {
		if (isdigit(*p) || ((*p) == '?') || ((*p) == '*') ||
			((*p) == '.'))
		{
			p++;
		}
		return 0;
	}
	return 1;
}

void refuse(ConnectionInfo *c, int logCode)
{
	closesocket(c->reFd);
	c->reClosed = 1;
	c->loClosed = 1;
	c->coClosed = 1;
	logEvent(c, c->server, logCode);
}

RETSIGTYPE term(int s)
{
	/* Obey the request, but first flush the log */
	if (logFile) {
		fclose(logFile);
	}
	exit(0);
}

