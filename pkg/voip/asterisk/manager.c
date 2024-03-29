/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2005, Digium, Inc.
 *
 * Mark Spencer <markster@digium.com>
 *
 * See http://www.asterisk.org for more information about
 * the Asterisk project. Please do not directly contact
 * any of the maintainers of this project for assistance;
 * the project provides a web site, mailing lists and IRC
 * channels for your use.
 *
 * This program is free software, distributed under the terms of
 * the GNU General Public License Version 2. See the LICENSE file
 * at the top of the source tree.
 */

/*! \file
 *
 * \brief The Asterisk Management Interface - AMI
 *
 * Channel Management and more
 * 
 * \ref amiconf
 */

/*! \addtogroup Group_AMI AMI functions 
*/
/*! @{ 
 Doxygen group */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <errno.h>
#include <unistd.h>

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 1.9.2.1.2.28.4.1 $")

#include "asterisk/channel.h"
#include "asterisk/file.h"
#include "asterisk/manager.h"
#include "asterisk/config.h"
#include "asterisk/callerid.h"
#include "asterisk/lock.h"
#include "asterisk/logger.h"
#include "asterisk/options.h"
#include "asterisk/cli.h"
#include "asterisk/app.h"
#include "asterisk/pbx.h"
#include "asterisk/md5.h"
#include "asterisk/acl.h"
#include "asterisk/utils.h"
#include "asterisk/causes.h"

#define NATIVE_FORMATS ((AST_FORMAT_MAX_AUDIO << 1) - 1)

struct fast_originate_helper {
	char tech[256];
	char data[256];
	int timeout;
	char app[256];
	char appdata[256];
	char cid_name[256];
	char cid_num[256];
	char context[256];
	char exten[256];
	char idtext[256];
	int priority;
	struct ast_variable *vars;
};

struct finish_async_originate_helper {
    struct ast_channel **channels;
    struct ast_channel **live_channels;
    int to;
    int exten_count;
    int live_channels_count;
    char *dial_number;
    int pi;
    char idtext[256];
    struct mansession *session;
};

struct service_dial_data {
    char trunk[256];
    char exten[256];
	char action_id[256];
	int timeout;
};

struct diag_outcall_data {
    char trunk[256];
    char exten[256];
	char action_id[256];
	int timeout;
	int require_answer;
};

typedef enum {
    	DIAGNOSTICS_IN_PROGRESS = 0,
	DIAGNOSTICS_SUCCESS, 
	DIAGNOSTICS_TIMEDOUT,
	DIAGNOSTICS_CANCELED,
	DIAGNOSTICS_FAILED,
	DIAGNOSTICS_ERROR
} diagnostics_result ;

static unsigned char conf_file_md5[MD5_DIGEST_LEN];

static int enabled = 0;
static int portno = DEFAULT_MANAGER_PORT;
static int asock = -1;
static int displayconnects = 1;

static pthread_t originate_thread = AST_PTHREADT_NULL;
static pthread_t service_dial_thread = AST_PTHREADT_NULL;
static pthread_t diag_outcall_thread = AST_PTHREADT_NULL;

static pthread_t t;
AST_MUTEX_DEFINE_STATIC(sessionlock);
static int block_sockets = 0;

static struct permalias {
	int num;
	char *label;
} perms[] = {
	{ EVENT_FLAG_SYSTEM, "system" },
	{ EVENT_FLAG_CALL, "call" },
	{ EVENT_FLAG_LOG, "log" },
	{ EVENT_FLAG_VERBOSE, "verbose" },
	{ EVENT_FLAG_COMMAND, "command" },
	{ EVENT_FLAG_AGENT, "agent" },
	{ EVENT_FLAG_USER, "user" },
	{ -1, "all" },
	{ 0, "none" },
};

static struct mansession *sessions = NULL;
static struct manager_action *first_action = NULL;
AST_MUTEX_DEFINE_STATIC(actionlock);

/*! If you are calling ast_carefulwrite, it is assumed that you are calling
   it on a file descriptor that _DOES_ have NONBLOCK set.  This way,
   there is only one system call made to do a write, unless we actually
   have a need to wait.  This way, we get better performance. */
int ast_carefulwrite(int fd, char *s, int len, int timeoutms) 
{
	/* Try to write string, but wait no more than ms milliseconds
	   before timing out */
	int res=0;
	struct pollfd fds[1];
	while(len) {
		res = write(fd, s, len);
		if ((res < 0) && (errno != EAGAIN)) {
			return -1;
		}
		if (res < 0) res = 0;
		len -= res;
		s += res;
		res = 0;
		if (len) {
			fds[0].fd = fd;
			fds[0].events = POLLOUT;
			/* Wait until writable again */
			res = poll(fds, 1, timeoutms);
			if (res < 1)
				return -1;
		}
	}
	return res;
}

/*! authority_to_str: Convert authority code to string with serveral options */
static char *authority_to_str(int authority, char *res, int reslen)
{
	int running_total = 0, i;
	memset(res, 0, reslen);
	for (i=0; i<sizeof(perms) / sizeof(perms[0]) - 1; i++) {
		if (authority & perms[i].num) {
			if (*res) {
				strncat(res, ",", (reslen > running_total) ? reslen - running_total : 0);
				running_total++;
			}
			strncat(res, perms[i].label, (reslen > running_total) ? reslen - running_total : 0);
			running_total += strlen(perms[i].label);
		}
	}
	if (ast_strlen_zero(res)) {
		ast_copy_string(res, "<none>", reslen);
	}
	return res;
}

static char *complete_show_mancmd(char *line, char *word, int pos, int state)
{
	struct manager_action *cur = first_action;
	int which = 0;

	ast_mutex_lock(&actionlock);
	while (cur) { /* Walk the list of actions */
		if (!strncasecmp(word, cur->action, strlen(word))) {
			if (++which > state) {
				char *ret = strdup(cur->action);
				ast_mutex_unlock(&actionlock);
				return ret;
			}
		}
		cur = cur->next;
	}
	ast_mutex_unlock(&actionlock);
	return NULL;
}

static int handle_showmancmd(int fd, int argc, char *argv[])
{
	struct manager_action *cur = first_action;
	char authority[80];
	int num;

	if (argc != 4)
		return RESULT_SHOWUSAGE;
	ast_mutex_lock(&actionlock);
	while (cur) { /* Walk the list of actions */
		for (num = 3; num < argc; num++) {
			if (!strcasecmp(cur->action, argv[num])) {
				ast_cli(fd, "Action: %s\nSynopsis: %s\nPrivilege: %s\n%s\n", cur->action, cur->synopsis, authority_to_str(cur->authority, authority, sizeof(authority) -1), cur->description ? cur->description : "");
			}
		}
		cur = cur->next;
	}

	ast_mutex_unlock(&actionlock);
	return RESULT_SUCCESS;
}

/*! \brief  handle_showmancmds: CLI command */
/* Should change to "manager show commands" */
static int handle_showmancmds(int fd, int argc, char *argv[])
{
	struct manager_action *cur = first_action;
	char authority[80];
	char *format = "  %-15.15s  %-15.15s  %-55.55s\n";

	ast_mutex_lock(&actionlock);
	ast_cli(fd, format, "Action", "Privilege", "Synopsis");
	ast_cli(fd, format, "------", "---------", "--------");
	while (cur) { /* Walk the list of actions */
		ast_cli(fd, format, cur->action, authority_to_str(cur->authority, authority, sizeof(authority) -1), cur->synopsis);
		cur = cur->next;
	}

	ast_mutex_unlock(&actionlock);
	return RESULT_SUCCESS;
}

/*! \brief  handle_showmanconn: CLI command show manager connected */
/* Should change to "manager show connected" */
static int handle_showmanconn(int fd, int argc, char *argv[])
{
	struct mansession *s;
	char iabuf[INET_ADDRSTRLEN];
	char *format = "  %-15.15s  %-15.15s\n";
	ast_mutex_lock(&sessionlock);
	s = sessions;
	ast_cli(fd, format, "Username", "IP Address");
	while (s) {
		ast_cli(fd, format,s->username, ast_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
		s = s->next;
	}

	ast_mutex_unlock(&sessionlock);
	return RESULT_SUCCESS;
}

static char showmancmd_help[] = 
"Usage: show manager command <actionname>\n"
"	Shows the detailed description for a specific Asterisk manager interface command.\n";

static char showmancmds_help[] = 
"Usage: show manager commands\n"
"	Prints a listing of all the available Asterisk manager interface commands.\n";

static char showmanconn_help[] = 
"Usage: show manager connected\n"
"	Prints a listing of the users that are currently connected to the\n"
"Asterisk manager interface.\n";

static struct ast_cli_entry show_mancmd_cli =
	{ { "show", "manager", "command", NULL },
	handle_showmancmd, "Show a manager interface command", showmancmd_help, complete_show_mancmd };

static struct ast_cli_entry show_mancmds_cli =
	{ { "show", "manager", "commands", NULL },
	handle_showmancmds, "List manager interface commands", showmancmds_help };

static struct ast_cli_entry show_manconn_cli =
	{ { "show", "manager", "connected", NULL },
	handle_showmanconn, "Show connected manager interface users", showmanconn_help };

static void free_session(struct mansession *s)
{
	struct eventqent *eqe;
	if (s->fd > -1)
		close(s->fd);
	ast_mutex_destroy(&s->__lock);
	while(s->eventq) {
		eqe = s->eventq;
		s->eventq = s->eventq->next;
		free(eqe);
	}
	free(s);
}

static void destroy_session(struct mansession *s)
{
	struct mansession *cur, *prev = NULL;
	ast_mutex_lock(&sessionlock);
	cur = sessions;
	while(cur) {
		if (cur == s)
			break;
		prev = cur;
		cur = cur->next;
	}
	if (cur) {
		if (prev)
			prev->next = cur->next;
		else
			sessions = cur->next;
		free_session(s);
	} else
		ast_log(LOG_WARNING, "Trying to delete nonexistent session %p?\n", s);
	ast_mutex_unlock(&sessionlock);
	
}

char *astman_get_header(struct message *m, char *var)
{
	char cmp[80];
	int x;
	snprintf(cmp, sizeof(cmp), "%s: ", var);
	for (x=0;x<m->hdrcount;x++)
		if (!strncasecmp(cmp, m->headers[x], strlen(cmp)))
			return m->headers[x] + strlen(cmp);
	return "";
}

struct ast_variable *astman_get_variables(struct message *m)
{
	int varlen, x, y;
	struct ast_variable *head = NULL, *cur;
	char *var, *val;
	unsigned int var_count;
        char *vars[32];
	
	varlen = strlen("Variable: ");	

	for (x = 0; x < m->hdrcount; x++) {
		if (strncasecmp("Variable: ", m->headers[x], varlen))
			continue;

		if (!(var = ast_strdupa(m->headers[x] + varlen)))
			return head;

		if ((var_count = ast_app_separate_args(var, '|', vars, sizeof(vars) / sizeof(vars[0])))) {
			for (y = 0; y < var_count; y++) {
				if (!vars[y])
					continue;
				var = val = ast_strdupa(vars[y]);
				strsep(&val, "=");
				if (!val || ast_strlen_zero(var))
					continue;
				cur = ast_variable_new(var, val);
				if (head) {
					cur->next = head;
					head = cur;
				} else
					head = cur;
			}
		}
	}

	return head;
}

/*! NOTE:
   Callers of astman_send_error(), astman_send_response() or astman_send_ack() must EITHER
   hold the session lock _or_ be running in an action callback (in which case s->busy will
   be non-zero). In either of these cases, there is no need to lock-protect the session's
   fd, since no other output will be sent (events will be queued), and no input will
   be read until either the current action finishes or get_input() obtains the session
   lock.
 */
void astman_send_error(struct mansession *s, struct message *m, char *error)
{
	char *id = astman_get_header(m,"ActionID");

	ast_cli(s->fd, "Response: Error\r\n");
	if (!ast_strlen_zero(id))
		ast_cli(s->fd, "ActionID: %s\r\n",id);
	ast_cli(s->fd, "Message: %s\r\n\r\n", error);
}

void astman_send_response(struct mansession *s, struct message *m, char *resp, char *msg)
{
	char *id = astman_get_header(m,"ActionID");

	ast_cli(s->fd, "Response: %s\r\n", resp);
	if (!ast_strlen_zero(id))
		ast_cli(s->fd, "ActionID: %s\r\n",id);
	if (msg)
		ast_cli(s->fd, "Message: %s\r\n\r\n", msg);
	else
		ast_cli(s->fd, "\r\n");
}

void astman_send_ack(struct mansession *s, struct message *m, char *msg)
{
	astman_send_response(s, m, "Success", msg);
}

/*! Tells you if smallstr exists inside bigstr
   which is delim by delim and uses no buf or stringsep
   ast_instring("this|that|more","this",',') == 1;

   feel free to move this to app.c -anthm */
static int ast_instring(char *bigstr, char *smallstr, char delim) 
{
	char *val = bigstr, *next;

	do {
		if ((next = strchr(val, delim))) {
			if (!strncmp(val, smallstr, (next - val)))
				return 1;
			else
				continue;
		} else
			return !strcmp(smallstr, val);

	} while (*(val = (next + 1)));

	return 0;
}

static int get_perm(char *instr)
{
	int x = 0, ret = 0;

	if (!instr)
		return 0;

	for (x=0; x<sizeof(perms) / sizeof(perms[0]); x++)
		if (ast_instring(instr, perms[x].label, ','))
			ret |= perms[x].num;
	
	return ret;
}

static int ast_is_number(char *string) 
{
	int ret = 1, x = 0;

	if (!string)
		return 0;

	for (x=0; x < strlen(string); x++) {
		if (!(string[x] >= 48 && string[x] <= 57)) {
			ret = 0;
			break;
		}
	}
	
	return ret ? atoi(string) : 0;
}

static int ast_strings_to_mask(char *string) 
{
	int x, ret = -1;
	
	x = ast_is_number(string);

	if (x) {
		ret = x;
	} else if (ast_strlen_zero(string)) {
		ret = -1;
	} else if (ast_false(string)) {
		ret = 0;
	} else if (ast_true(string)) {
		ret = 0;
		for (x=0; x<sizeof(perms) / sizeof(perms[0]); x++)
			ret |= perms[x].num;		
	} else {
		ret = 0;
		for (x=0; x<sizeof(perms) / sizeof(perms[0]); x++) {
			if (ast_instring(string, perms[x].label, ',')) 
				ret |= perms[x].num;		
		}
	}

	return ret;
}

/*! 
   Rather than braindead on,off this now can also accept a specific int mask value 
   or a ',' delim list of mask strings (the same as manager.conf) -anthm
*/

static int set_eventmask(struct mansession *s, char *eventmask)
{
	int maskint = ast_strings_to_mask(eventmask);

	ast_mutex_lock(&s->__lock);
	if (maskint >= 0)	
		s->send_events = maskint;
	ast_mutex_unlock(&s->__lock);
	
	return maskint;
}

static int authenticate(struct mansession *s, struct message *m)
{
	struct ast_config *cfg;
	char iabuf[INET_ADDRSTRLEN];
	char *cat;
	char *user = astman_get_header(m, "Username");
	char *pass = astman_get_header(m, "Secret");
	char *authtype = astman_get_header(m, "AuthType");
	char *key = astman_get_header(m, "Key");
	char *events = astman_get_header(m, "Events");
	
	cfg = ast_config_load("manager.conf");
	if (!cfg)
		return -1;
	cat = ast_category_browse(cfg, NULL);
	while(cat) {
		if (strcasecmp(cat, "general")) {
			/* This is a user */
			if (!strcasecmp(cat, user)) {
				struct ast_variable *v;
				struct ast_ha *ha = NULL;
				char *password = NULL;
				v = ast_variable_browse(cfg, cat);
				while (v) {
					if (!strcasecmp(v->name, "secret")) {
						password = v->value;
					} else if (!strcasecmp(v->name, "permit") ||
						   !strcasecmp(v->name, "deny")) {
						ha = ast_append_ha(v->name, v->value, ha);
					} else if (!strcasecmp(v->name, "writetimeout")) {
						int val = atoi(v->value);

						if (val < 100)
							ast_log(LOG_WARNING, "Invalid writetimeout value '%s' at line %d\n", v->value, v->lineno);
						else
							s->writetimeout = val;
					}
				    		
					v = v->next;
				}
				if (ha && !ast_apply_ha(ha, &(s->sin))) {
					ast_log(LOG_NOTICE, "%s failed to pass IP ACL as '%s'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr), user);
					ast_free_ha(ha);
					ast_config_destroy(cfg);
					return -1;
				} else if (ha)
					ast_free_ha(ha);
				if (!strcasecmp(authtype, "MD5")) {
					if (!ast_strlen_zero(key) && s->challenge) {
						int x;
						int len=0;
						char md5key[256] = "";
						struct MD5Context md5;
						unsigned char digest[16];
						MD5Init(&md5);
						MD5Update(&md5, (unsigned char *) s->challenge, strlen(s->challenge));
						MD5Update(&md5, (unsigned char *) password, strlen(password));
						MD5Final(digest, &md5);
						for (x=0;x<16;x++)
							len += sprintf(md5key + len, "%2.2x", digest[x]);
						if (!strcmp(md5key, key))
							break;
						else {
							ast_config_destroy(cfg);
							return -1;
						}
					}
				} else if (password && !strcasecmp(password, pass)) {
					break;
				} else {
					ast_log(LOG_NOTICE, "%s failed to authenticate as '%s'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr), user);
					ast_config_destroy(cfg);
					return -1;
				}	
			}
		}
		cat = ast_category_browse(cfg, cat);
	}
	if (cat) {
		ast_copy_string(s->username, cat, sizeof(s->username));
		s->readperm = get_perm(ast_variable_retrieve(cfg, cat, "read"));
		s->writeperm = get_perm(ast_variable_retrieve(cfg, cat, "write"));
		ast_config_destroy(cfg);
		if (events)
			set_eventmask(s, events);
		return 0;
	}
	ast_log(LOG_NOTICE, "%s tried to authenticate with nonexistent user '%s'\n", ast_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr), user);
	ast_config_destroy(cfg);
	return -1;
}

/*! \brief PING: Manager PING */
static char mandescr_ping[] = 
"Description: A 'Ping' action will ellicit a 'Pong' response.  Used to keep the "
"  manager connection open.\n"
"Variables: NONE\n";

static int action_ping(struct mansession *s, struct message *m)
{
	astman_send_response(s, m, "Pong", NULL);
	return 0;
}

static char mandescr_listcommands[] = 
"Description: Returns the action name and synopsis for every\n"
"  action that is available to the user\n"
"Variables: NONE\n";

static int action_listcommands(struct mansession *s, struct message *m)
{
	struct manager_action *cur = first_action;
	char idText[256] = "";
	char temp[BUFSIZ];
	char *id = astman_get_header(m,"ActionID");

	if (!ast_strlen_zero(id))
		snprintf(idText,256,"ActionID: %s\r\n",id);
	ast_cli(s->fd, "Response: Success\r\n%s", idText);
	ast_mutex_lock(&actionlock);
	while (cur) { /* Walk the list of actions */
		if ((s->writeperm & cur->authority) == cur->authority)
			ast_cli(s->fd, "%s: %s (Priv: %s)\r\n", cur->action, cur->synopsis, authority_to_str(cur->authority, temp, sizeof(temp)) );
		cur = cur->next;
	}
	ast_mutex_unlock(&actionlock);
	ast_cli(s->fd, "\r\n");

	return 0;
}

static char mandescr_events[] = 
"Description: Enable/Disable sending of events to this manager\n"
"  client.\n"
"Variables:\n"
"	EventMask: 'on' if all events should be sent,\n"
"		'off' if no events should be sent,\n"
"		'system,call,log' to select which flags events should have to be sent.\n";

static int action_events(struct mansession *s, struct message *m)
{
	char *mask = astman_get_header(m, "EventMask");
	int res;

	res = set_eventmask(s, mask);
	if (res > 0)
		astman_send_response(s, m, "Events On", NULL);
	else if (res == 0)
		astman_send_response(s, m, "Events Off", NULL);

	return 0;
}

static char mandescr_logoff[] = 
"Description: Logoff this manager session\n"
"Variables: NONE\n";

static int action_logoff(struct mansession *s, struct message *m)
{
	astman_send_response(s, m, "Goodbye", "Thanks for all the fish.");
	return -1;
}

static char mandescr_hangup[] = 
"Description: Hangup a channel\n"
"Variables: \n"
"	Channel: The channel name to be hungup\n";

static int action_hangup(struct mansession *s, struct message *m)
{
	struct ast_channel *c = NULL;
	char *name = astman_get_header(m, "Channel");
	if (ast_strlen_zero(name)) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	c = ast_get_channel_by_name_locked(name);
	if (!c) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	ast_softhangup(c, AST_SOFTHANGUP_EXPLICIT);
	ast_mutex_unlock(&c->lock);
	astman_send_ack(s, m, "Channel Hungup");
	return 0;
}

static char mandescr_setvar[] = 
"Description: Set a global or local channel variable.\n"
"Variables: (Names marked with * are required)\n"
"	Channel: Channel to set variable for\n"
"	*Variable: Variable name\n"
"	*Value: Value\n";

static int action_setvar(struct mansession *s, struct message *m)
{
        struct ast_channel *c = NULL;
        char *name = astman_get_header(m, "Channel");
        char *varname = astman_get_header(m, "Variable");
        char *varval = astman_get_header(m, "Value");
	
	if (ast_strlen_zero(varname)) {
		astman_send_error(s, m, "No variable specified");
		return 0;
	}
	
	if (ast_strlen_zero(varval)) {
		astman_send_error(s, m, "No value specified");
		return 0;
	}

	if (!ast_strlen_zero(name)) {
		c = ast_get_channel_by_name_locked(name);
		if (!c) {
			astman_send_error(s, m, "No such channel");
			return 0;
		}
	}
	
	pbx_builtin_setvar_helper(c, varname, varval);
	  
	if (c)
		ast_mutex_unlock(&c->lock);

	astman_send_ack(s, m, "Variable Set");	

	return 0;
}

static char mandescr_getvar[] = 
"Description: Get the value of a global or local channel variable.\n"
"Variables: (Names marked with * are required)\n"
"	Channel: Channel to read variable from\n"
"	*Variable: Variable name\n"
"	ActionID: Optional Action id for message matching.\n";

static int action_getvar(struct mansession *s, struct message *m)
{
        struct ast_channel *c = NULL;
        char *name = astman_get_header(m, "Channel");
        char *varname = astman_get_header(m, "Variable");
	char *id = astman_get_header(m,"ActionID");
	char *varval;
	char *varval2=NULL;

	if (!strlen(varname)) {
		astman_send_error(s, m, "No variable specified");
		return 0;
	}

	if (strlen(name)) {
		c = ast_get_channel_by_name_locked(name);
		if (!c) {
			astman_send_error(s, m, "No such channel");
			return 0;
		}
	}
	
	varval=pbx_builtin_getvar_helper(c,varname);
	if (varval)
		varval2 = ast_strdupa(varval);
	if (!varval2)
		varval2 = "";
	if (c)
		ast_mutex_unlock(&c->lock);
	ast_cli(s->fd, "Response: Success\r\n"
		"Variable: %s\r\nValue: %s\r\n" ,varname,varval2);
	if (!ast_strlen_zero(id))
		ast_cli(s->fd, "ActionID: %s\r\n",id);
	ast_cli(s->fd, "\r\n");

	return 0;
}


/*! \brief  action_status: Manager "status" command to show channels */
/* Needs documentation... */
static int action_status(struct mansession *s, struct message *m)
{
	char *id = astman_get_header(m,"ActionID");
    	char *name = astman_get_header(m,"Channel");
	char idText[256] = "";
	struct ast_channel *c;
	char bridge[256];
	struct timeval now = ast_tvfromboot();
	long elapsed_seconds=0;
	int all = ast_strlen_zero(name); /* set if we want all channels */

	astman_send_ack(s, m, "Channel status will follow");
        if (!ast_strlen_zero(id))
                snprintf(idText,256,"ActionID: %s\r\n",id);
	if (all)
		c = ast_channel_walk_locked(NULL);
	else {
		c = ast_get_channel_by_name_locked(name);
		if (!c) {
			astman_send_error(s, m, "No such channel");
			return 0;
		}
	}
	/* if we look by name, we break after the first iteration */
	while(c) {
		if (c->_bridge)
			snprintf(bridge, sizeof(bridge), "Link: %s\r\n", c->_bridge->name);
		else
			bridge[0] = '\0';
		if (c->pbx) {
			if (c->cdr) {
				elapsed_seconds = now.tv_sec - c->cdr->start.tv_sec;
			}
			ast_cli(s->fd,
			"Event: Status\r\n"
			"Privilege: Call\r\n"
			"Channel: %s\r\n"
			"CallerID: %s\r\n"
			"CallerIDName: %s\r\n"
			"Account: %s\r\n"
			"State: %s\r\n"
			"Context: %s\r\n"
			"Extension: %s\r\n"
			"Priority: %d\r\n"
			"Seconds: %ld\r\n"
			"%s"
			"Uniqueid: %s\r\n"
			"%s"
			"\r\n",
			c->name, 
			c->cid.cid_num ? c->cid.cid_num : "<unknown>", 
			c->cid.cid_name ? c->cid.cid_name : "<unknown>", 
			c->accountcode,
			ast_state2str(c->_state, ast_test_flag(c, AST_FLAG_CALL_ONHOLD)), c->context,
			c->exten, c->priority, (long)elapsed_seconds, bridge, c->uniqueid, idText);
		} else {
			ast_cli(s->fd,
			"Event: Status\r\n"
			"Privilege: Call\r\n"
			"Channel: %s\r\n"
			"CallerID: %s\r\n"
			"CallerIDName: %s\r\n"
			"Account: %s\r\n"
			"State: %s\r\n"
			"%s"
			"Uniqueid: %s\r\n"
			"%s"
			"\r\n",
			c->name, 
			c->cid.cid_num ? c->cid.cid_num : "<unknown>", 
			c->cid.cid_name ? c->cid.cid_name : "<unknown>", 
			c->accountcode,
			ast_state2str(c->_state, ast_test_flag(c, AST_FLAG_CALL_ONHOLD)), bridge, c->uniqueid, idText);
		}
		ast_mutex_unlock(&c->lock);
		if (!all)
			break;
		c = ast_channel_walk_locked(c);
	}
	ast_cli(s->fd,
	"Event: StatusComplete\r\n"
	"%s"
	"\r\n",idText);
	return 0;
}

static char mandescr_redirect[] = 
"Description: Redirect (transfer) a call.\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Channel to redirect\n"
"	ExtraChannel: Second call leg to transfer (optional)\n"
"	*Exten: Extension to transfer to\n"
"	*Context: Context to transfer to\n"
"	*Priority: Priority to transfer to\n"
"	ActionID: Optional Action id for message matching.\n";

/*! \brief  action_redirect: The redirect manager command */
static int action_redirect(struct mansession *s, struct message *m)
{
	char *name = astman_get_header(m, "Channel");
	char *name2 = astman_get_header(m, "ExtraChannel");
	char *exten = astman_get_header(m, "Exten");
	char *context = astman_get_header(m, "Context");
	char *priority = astman_get_header(m, "Priority");
	struct ast_channel *chan, *chan2 = NULL;
	int pi = 0;
	int res;

	if (ast_strlen_zero(name)) {
		astman_send_error(s, m, "Channel not specified");
		return 0;
	}
	if (!ast_strlen_zero(priority) && (sscanf(priority, "%d", &pi) != 1)) {
		astman_send_error(s, m, "Invalid priority\n");
		return 0;
	}
	chan = ast_get_channel_by_name_locked(name);
	if (!chan) {
		char buf[BUFSIZ];
		snprintf(buf, sizeof(buf), "Channel does not exist: %s", name);
		astman_send_error(s, m, buf);
		return 0;
	}
	if (!ast_strlen_zero(name2))
		chan2 = ast_get_channel_by_name_locked(name2);
	res = ast_async_goto(chan, context, exten, pi);
	if (!res) {
		if (!ast_strlen_zero(name2)) {
			if (chan2)
				res = ast_async_goto(chan2, context, exten, pi);
			else
				res = -1;
			if (!res)
				astman_send_ack(s, m, "Dual Redirect successful");
			else
				astman_send_error(s, m, "Secondary redirect failed");
		} else
			astman_send_ack(s, m, "Redirect successful");
	} else
		astman_send_error(s, m, "Redirect failed");
	if (chan)
		ast_mutex_unlock(&chan->lock);
	if (chan2)
		ast_mutex_unlock(&chan2->lock);
	return 0;
}

static char mandescr_command[] = 
"Description: Run a CLI command.\n"
"Variables: (Names marked with * are required)\n"
"	*Command: Asterisk CLI command to run\n"
"	ActionID: Optional Action id for message matching.\n";

/*! \brief  action_command: Manager command "command" - execute CLI command */
static int action_command(struct mansession *s, struct message *m)
{
	char *cmd = astman_get_header(m, "Command");
	char *id = astman_get_header(m, "ActionID");
	ast_cli(s->fd, "Response: Follows\r\nPrivilege: Command\r\n");
	if (!ast_strlen_zero(id))
		ast_cli(s->fd, "ActionID: %s\r\n", id);
	/* FIXME: Wedge a ActionID response in here, waiting for later changes */
	ast_cli_command(s->fd, cmd);
	ast_cli(s->fd, "--END COMMAND--\r\n\r\n");
	return 0;
}

static int prepare_extensions(char *exten,
    struct fast_originate_helper **dial_extensions, int *exten_count)
{
    char *cur = exten, *rest, *number, *tech;
    struct fast_originate_helper *extensions = NULL, *cur_ext;
    int count = 0;
    *dial_extensions = NULL;

    if(ast_strlen_zero(exten))
		goto Error;
    /* count extensions */
    do 
    {
		/* Remember where to start next time */
		rest = strchr(cur, '&');
		if (rest)
	    	rest++;
		count++;
		cur = rest;
    } while (cur);
    
    if (count < 1)
		goto Error;

    extensions = (struct fast_originate_helper *)malloc
		(sizeof(struct fast_originate_helper) * count);
    if (!extensions)
		goto Error;
    /* Parse again- fill data */
    cur_ext = extensions;
    cur = exten;
    do
    {
		/* Remember where to start next time */
		rest = strchr(cur, '&');
		if (rest)
		{
	   	 *rest = 0;
	   	 rest++;
		}
		/* Get a technology/[device:]number pair */
		tech = cur;
		number = strchr(tech, '/');
		if (!number)
		{
	    	ast_log(LOG_DEBUG, "Dial argument takes format"
			"(jdsp/number)\n");
	    	goto Error;
		}
		*number = '\0';
		number++;
		memset(cur_ext, 0, sizeof(struct fast_originate_helper));
		/* copy data and add 'C' char
		 * This is done for callback functionality, introduced in
		 * B104432, R102476, R103124. Relevant only for jdsp extensions.
		 * Also see R135388, R145598
		*/
		ast_copy_string(cur_ext->tech, tech,256);
		snprintf (cur_ext->data, 256, "%s%s", CALLBACK_MAGIC, number);
		ast_log(LOG_DEBUG, "tech=%s, data =%s, cur=%s,rest=%s\n",cur_ext->tech, cur_ext->data,cur,rest);
		cur_ext++;
		cur = rest;
    } while (cur);

    ast_log(LOG_DEBUG,"filled all extensions requested, total=%d\n",count);
    *exten_count = count;
    *dial_extensions = extensions;
    return 0;
Error:
    if (extensions)
	free(extensions);
    return -1;
}

static void hangup_channels (struct ast_channel **channels, int count,
    struct ast_channel *skip_channel)
{
    int i;

    for (i = 0; i < count; i++)
    {
		if (channels[i] && (channels[i] != skip_channel))
		{
	    	if (ast_test_flag(channels[i], AST_FLAG_OFFHOOK))
				channels[i]->_softhangup = 1;
	    	else
				ast_hangup(channels[i]);
		}
    } 
}

static int get_offhooked_extensions(struct ast_channel **channels,
					   int count,
					   struct ast_channel **first_offhookchan)
{
    int i, offhook_count = 0;
    
	for (i = 0; i < count; i++)
    {
		/* no channel created- can be busy or offhook */
		if (!channels[i])
	   		continue;
		if (ast_test_flag(channels[i], AST_FLAG_OFFHOOK))
		{
	    	if (!*first_offhookchan)
				*first_offhookchan = channels[i];
	   		offhook_count++;
		}
    }
    return offhook_count;
}

static int wait_for_answer(struct ast_channel **live_channels, int chan_count,
	int to, int *outstate, struct ast_channel **asnwered_channel)
{
	int state = -1;
	struct ast_frame *f;
	int res = -1;
	struct ast_channel *peer = NULL;


	while (to)
	{
	    peer = ast_waitfor_n(live_channels, chan_count, &to);
	    if(to < 0)
		to = 0; /* mark time out, but try to get frame one last time */
	    if(peer)
	    {

			f = ast_read(peer);
			if (!f) 
			{
		    	state = AST_CONTROL_HANGUP;
		    	break;
			}

			if (f->frametype == AST_FRAME_CONTROL)
			{
		    	if (f->subclass == AST_CONTROL_CALLBACK_CANCEL)
				{
				    /* cancel request - return error */
			    	ast_frfree(f);
			    	return -2;
				}
				else if (f->subclass == AST_CONTROL_RINGING)
					state = AST_CONTROL_RINGING;
		    	else if ((f->subclass == AST_CONTROL_BUSY) ||
					(f->subclass == AST_CONTROL_CONGESTION))
		    	{
					state = f->subclass;
					ast_frfree(f);
					break;
		    	}
		    	else if (f->subclass == AST_CONTROL_ANSWER)
		    	{
					state = f->subclass;
					ast_frfree(f);
					res = 0;
					*asnwered_channel = peer;
					break;
		    	}
		    	else if (f->subclass == AST_CONTROL_PROGRESS)
		    	{
					/* Ignore */
		    	}
		    	else if (f->subclass == -1)
		    	{
					/* Ignore -- just stopping indications */
		    	}
		    	else
		    	{
					ast_log(LOG_NOTICE, "Don't know what to do with control"
			    		" frame %d\n", f->subclass);
		    	}
			}
			ast_frfree(f);

			if (peer->_state == AST_STATE_UP) 
			{
		    	state = AST_CONTROL_ANSWER;
		    	res = 0;
		    	*asnwered_channel = peer;
		    	break;
			}
	    }

	}
	if (outstate)
	    *outstate = state;
	return res;
}

static void *finish_async_originate(void *data)
{
    int state, wait_res;
    struct ast_channel *answered_channel = NULL;
    struct finish_async_originate_helper *originate_data =
		(struct finish_async_originate_helper *)data;
    ast_callback_frame_payload_t payload;

    /* wait for answer for all dialed extensions (if any) */
    if ((wait_res = wait_for_answer(originate_data->live_channels,
		originate_data->live_channels_count, originate_data->to, &state, 
		&answered_channel)))
    {
		if (wait_res == -2)
		{
			manager_event(EVENT_FLAG_CALL, "OriginateResp",
				"Response: Error\r\n"
		 		"%s"
		 		"Reason: Wait was canceled\r\n", originate_data->idtext);
		}
		else
		{
			manager_event(EVENT_FLAG_CALL, "OriginateResp",
				"Response: Error\r\n"
		        	"%s"
		        	"Reason: Handsets %s\r\n", originate_data->idtext,
		        	state == AST_CONTROL_RINGING ? "ringing" : "busy");
		}
		goto Exit;
    }

    if (!answered_channel)
    {
		manager_event(EVENT_FLAG_CALL, "OriginateResp",
		    "Response: Error\r\n"
	    	"%s"
	    	"Reason: Failed to establish a call\r\n", originate_data->idtext);
		goto Exit;
    }

    /* Final fixups */
    if (originate_data->dial_number)
    {
		ast_copy_string(answered_channel->exten, originate_data->dial_number,
	    	sizeof(answered_channel->exten));
    }
    answered_channel->priority = originate_data->pi;

    /* Indicate to channel that callback is in progress - for incoming call
     * return busy and start voice for case when there is no bridge, like in a
     * case with Playback() in wake up feature */
    payload.state = AST_CALLBACK_IN_PROGRESS;
    ast_indicate_data(answered_channel, AST_CONTROL_CALLBACK, &payload,
	sizeof(ast_callback_frame_payload_t));
    if (ast_pbx_start(answered_channel) != AST_PBX_SUCCESS) 
    {
		manager_event(EVENT_FLAG_CALL, "OriginateResp",
		    "Response: Error\r\n"
	    	"%s"
	    	"Reason: Failed to establish a call\r\n", originate_data->idtext);
		answered_channel = NULL;
		goto Exit;
    }
    payload.state = AST_CALLBACK_ANSWERED;
    ast_indicate_data(answered_channel, AST_CONTROL_CALLBACK, &payload,
	sizeof(ast_callback_frame_payload_t));

	manager_event(EVENT_FLAG_CALL, "OriginateResp",
		"Response: Success\r\n"
		"%s"
		"Reason: OK\r\n", originate_data->idtext);
Exit:
	hangup_channels(originate_data->channels, originate_data->exten_count,
    	answered_channel);
    if(originate_data->live_channels)
		free(originate_data->live_channels);	
    if (originate_data->channels)
		free(originate_data->channels);	
    
    originate_data->session->channels = NULL;  
    if(originate_data->dial_number)
		free(originate_data->dial_number);
    if(originate_data)
		free(originate_data);
    return NULL;
}

static char mandescr_originate[] = 
"Description: Generates an outgoing call to a Extension/Context/Priority or\n"
"  Application/Data\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Channel name to call\n"
"	Exten: Extension to use (requires 'Context' and 'Priority')\n"
"	Context: Context to use (requires 'Exten' and 'Priority')\n"
"	Priority: Priority to use (requires 'Exten' and 'Context')\n"
"	Application: Application to use\n"
"	Data: Data to use (requires 'Application')\n"
"	Timeout: How long to wait for call to be answered (in ms)\n"
"	CallerID: Caller ID to be set on the outgoing channel\n"
"	Variable: Channel variable to set, multiple Variable: headers are allowed\n"
"	Account: Account code\n"
"	Async: Set to 'true' for fast origination\n";

static int action_originate(struct mansession *s, struct message *m)
{
	char *dial_number = astman_get_header(m, "Channel");
	char *exten = astman_get_header(m, "Exten");
	char *priority = astman_get_header(m, "Priority");
	char *timeout = astman_get_header(m, "Timeout");
	char *callerid = astman_get_header(m, "CallerID");
	char *id = astman_get_header(m, "ActionID");
	int pi = 0;
	int res;
	int to = 30000;
	struct fast_originate_helper *dial_extensions = NULL;
	struct finish_async_originate_helper *originate_data = NULL;
	int exten_count = 0;
	struct ast_channel **channels = NULL, **live_channels = NULL;
	struct ast_channel *asnwered_channel = NULL; 
	int i, j = 0, live_chn_size = 0, chan_count = 0;
	int number_offhooked_extensions = 0;
	struct ast_codec_pref slin;
    char *original_caller_id = NULL, *original_caller_name = NULL;
    pthread_attr_t attr;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    originate_thread = AST_PTHREADT_NULL;

	if (ast_strlen_zero(dial_number))
	{
		astman_send_error(s, m, "Number not specified");
		goto Exit;
	}
	if (!ast_strlen_zero(priority) && (sscanf(priority, "%d", &pi) != 1))
	{
		astman_send_error(s, m, "Invalid priority\n");
		goto Exit;
	}
	if (!ast_strlen_zero(timeout) && (sscanf(timeout, "%d", &to) != 1))
	{
		astman_send_error(s, m, "Invalid timeout\n");
		goto Exit;
	}

	ast_codec_pref_set2(&slin, NATIVE_FORMATS);

	if (prepare_extensions (exten, &dial_extensions, &exten_count))
	{
		astman_send_error(s, m, "Invalid extension(s)\n");
		goto Exit;
	}

	channels = (struct ast_channel **)malloc (sizeof(struct ast_channel*) *
		exten_count);
	if (!channels)
	{
		astman_send_error(s, m, "Out of memory\n");
		goto Exit;
	}
	for (i = 0; i < exten_count; i++)
	{
		int cause;
		channels[i] = ast_request(dial_extensions[i].tech, &slin,
			dial_extensions[i].data, &cause);	
	
		if (!channels[i])
		{
			/* If we can't, just go on to the next call */
			ast_log(LOG_DEBUG, "Unable to create channel of type '%s'\n"
				"(cause %d - %s)\n", dial_extensions[i].tech,
				cause, ast_cause2str(cause));
		}	
	}

	number_offhooked_extensions = get_offhooked_extensions(channels,
	    exten_count, &asnwered_channel);	
	switch (number_offhooked_extensions)
	{
	case 0:
		for (i = 0; i < exten_count; i++)
		{
			if(!channels[i])
				continue;

			/* copy original cid */
			original_caller_id = channels[i]->cid.cid_num? 
				strdup(channels[i]->cid.cid_num) :
				strdup("");
			original_caller_name = channels[i]->cid.cid_name?
				strdup(channels[i]->cid.cid_name) :
				strdup("");    	    	    
			ast_set_callerid(channels[i], NULL, callerid, NULL);
			res = ast_call(channels[i], dial_number, to);

			/* return the original cid */
			ast_set_callerid(channels[i],original_caller_id,
				original_caller_name, original_caller_id);
			if(res)
			{		
				ast_hangup(channels[i]);		
				continue;
			}       			
		}
		break;
	case 1:
		res = ast_call(asnwered_channel, dial_number, to);
		if (res)
		{ 
			astman_send_error(s, m, "Failed to make a call\n");
			ast_hangup(asnwered_channel);
			goto Exit; 
		}		
		goto Succes;
	default:
		/* more than one is offhook- ambiguous choice */
		astman_send_error(s, m, "Handsets busy\n");
		goto Exit;	
	} 

	/* copy only "live" channels */
	for(i = 0; i < exten_count; i++)
	{
	    if(channels[i])
			chan_count++;
	}

	live_chn_size = sizeof(struct ast_channel*) * chan_count;
	if (!live_chn_size)
	{
	    ast_log(LOG_DEBUG, " no channel was unable to make a call");
	    astman_send_error(s, m, "Handsets busy\n");
	    hangup_channels(channels, exten_count, NULL);
	    goto Exit;
	}
	live_channels = (struct ast_channel **)malloc(live_chn_size);
	
	for (i = 0; i < exten_count; i++)
	{
	    if (channels[i])
	    {
			live_channels[j] = channels[i];
			j++;
	    }
	}
	s->channels = live_channels;

	originate_data = (struct finish_async_originate_helper *)
    		malloc(sizeof(struct finish_async_originate_helper));
	originate_data->channels = channels;
	originate_data->live_channels = live_channels;
	originate_data->live_channels_count = chan_count;
	originate_data->to = to;
	originate_data->exten_count = exten_count;
	originate_data->dial_number = strdup(dial_number);
	originate_data->pi = pi;
	originate_data->session = s;
	if (!ast_strlen_zero(id))
	    snprintf(originate_data->idtext, sizeof(originate_data->idtext), "ActionID: %s\r\n", id);
	else
	    *originate_data->idtext = '\0';

	if (ast_pthread_create(&originate_thread, &attr, finish_async_originate,
	    (void *)originate_data))
	{
	    ast_log(LOG_ERROR, "Unable to run async call initiation \n");
	    astman_send_error(s, m, "Failed to establish a call \n");
	    hangup_channels(channels, exten_count, NULL);
	}
	 goto Exit;
 
Succes:
	/* hangup all other channels */
	hangup_channels(channels, exten_count, asnwered_channel);
	astman_send_ack(s, m, "Originate successfully queued");
Exit:
	if (original_caller_id)
		free(original_caller_id);
	if (original_caller_name)
		free(original_caller_name);
	if(dial_extensions)
		free(dial_extensions);
	pthread_attr_destroy(&attr);
	return 0;
}

static char mandescr_originate_cancel[] = 
"Description: Cancels the outgoing call request";

static int action_originate_cancel(struct mansession *s, struct message *m)
{    
    /* send CALLBACK_CANCEL frame to one of the queued channels */
    if(s->channels && s->channels[0])
		ast_queue_control(s->channels[0], AST_CONTROL_CALLBACK_CANCEL);
    ast_log(LOG_DEBUG, "Canceled action_originate \n");
    return 0;
}

/*! 	\brief Help text for manager command mailboxstatus
 */
static char mandescr_mailboxstatus[] = 
"Description: Checks a voicemail account for status.\n"
"Variables: (Names marked with * are required)\n"
"	*Mailbox: Full mailbox ID <mailbox>@<vm-context>\n"
"	ActionID: Optional ActionID for message matching.\n"
"Returns number of messages.\n"
"	Message: Mailbox Status\n"
"	Mailbox: <mailboxid>\n"
"	Waiting: <count>\n"
"\n";

static int action_mailboxstatus(struct mansession *s, struct message *m)
{
	char *mailbox = astman_get_header(m, "Mailbox");
	char *id = astman_get_header(m,"ActionID");
	char idText[256] = "";
	int ret;
	if (ast_strlen_zero(mailbox)) {
		astman_send_error(s, m, "Mailbox not specified");
		return 0;
	}
        if (!ast_strlen_zero(id))
                snprintf(idText,256,"ActionID: %s\r\n",id);
	ret = ast_app_has_voicemail(mailbox, NULL);
	ast_cli(s->fd, "Response: Success\r\n"
				   "%s"
				   "Message: Mailbox Status\r\n"
				   "Mailbox: %s\r\n"
		 		   "Waiting: %d\r\n\r\n", idText, mailbox, ret);
	return 0;
}

static char mandescr_mailboxcount[] = 
"Description: Checks a voicemail account for new messages.\n"
"Variables: (Names marked with * are required)\n"
"	*Mailbox: Full mailbox ID <mailbox>@<vm-context>\n"
"	ActionID: Optional ActionID for message matching.\n"
"Returns number of new and old messages.\n"
"	Message: Mailbox Message Count\n"
"	Mailbox: <mailboxid>\n"
"	NewMessages: <count>\n"
"	OldMessages: <count>\n"
"\n";
static int action_mailboxcount(struct mansession *s, struct message *m)
{
	char *mailbox = astman_get_header(m, "Mailbox");
	char *id = astman_get_header(m,"ActionID");
	char idText[256] = "";
	int newmsgs = 0, oldmsgs = 0;
	if (ast_strlen_zero(mailbox)) {
		astman_send_error(s, m, "Mailbox not specified");
		return 0;
	}
	ast_app_messagecount(mailbox, &newmsgs, &oldmsgs);
	if (!ast_strlen_zero(id)) {
		snprintf(idText,256,"ActionID: %s\r\n",id);
	}
	ast_cli(s->fd, "Response: Success\r\n"
				   "%s"
				   "Message: Mailbox Message Count\r\n"
				   "Mailbox: %s\r\n"
		 		   "NewMessages: %d\r\n"
				   "OldMessages: %d\r\n" 
				   "\r\n",
				    idText,mailbox, newmsgs, oldmsgs);
	return 0;
}

static char mandescr_extensionstate[] = 
"Description: Report the extension state for given extension.\n"
"  If the extension has a hint, will use devicestate to check\n"
"  the status of the device connected to the extension.\n"
"Variables: (Names marked with * are required)\n"
"	*Exten: Extension to check state on\n"
"	*Context: Context for extension\n"
"	ActionId: Optional ID for this transaction\n"
"Will return an \"Extension Status\" message.\n"
"The response will include the hint for the extension and the status.\n";

static int action_extensionstate(struct mansession *s, struct message *m)
{
	char *exten = astman_get_header(m, "Exten");
	char *context = astman_get_header(m, "Context");
	char *id = astman_get_header(m,"ActionID");
	char idText[256] = "";
	char hint[256] = "";
	int status;
	if (ast_strlen_zero(exten)) {
		astman_send_error(s, m, "Extension not specified");
		return 0;
	}
	if (ast_strlen_zero(context))
		context = "default";
	status = ast_extension_state(NULL, context, exten);
	ast_get_hint(hint, sizeof(hint) - 1, NULL, 0, NULL, context, exten);
        if (!ast_strlen_zero(id)) {
                snprintf(idText,256,"ActionID: %s\r\n",id);
        }
	ast_cli(s->fd, "Response: Success\r\n"
			           "%s"
				   "Message: Extension Status\r\n"
				   "Exten: %s\r\n"
				   "Context: %s\r\n"
				   "Hint: %s\r\n"
		 		   "Status: %d\r\n\r\n",
				   idText,exten, context, hint, status);
	return 0;
}

static char mandescr_timeout[] = 
"Description: Hangup a channel after a certain time.\n"
"Variables: (Names marked with * are required)\n"
"	*Channel: Channel name to hangup\n"
"	*Timeout: Maximum duration of the call (sec)\n"
"Acknowledges set time with 'Timeout Set' message\n";

static int action_timeout(struct mansession *s, struct message *m)
{
	struct ast_channel *c = NULL;
	char *name = astman_get_header(m, "Channel");
	int timeout = atoi(astman_get_header(m, "Timeout"));
	if (ast_strlen_zero(name)) {
		astman_send_error(s, m, "No channel specified");
		return 0;
	}
	if (!timeout) {
		astman_send_error(s, m, "No timeout specified");
		return 0;
	}
	c = ast_get_channel_by_name_locked(name);
	if (!c) {
		astman_send_error(s, m, "No such channel");
		return 0;
	}
	ast_channel_setwhentohangup(c, timeout);
	ast_mutex_unlock(&c->lock);
	astman_send_ack(s, m, "Timeout Set");
	return 0;
}

static int process_message(struct mansession *s, struct message *m)
{
	char action[80] = "";
	struct manager_action *tmp = first_action;
	char *id = astman_get_header(m,"ActionID");
	char idText[256] = "";
	char iabuf[INET_ADDRSTRLEN];
	char command[80] = "";

	ast_copy_string(action, astman_get_header(m, "Action"), sizeof(action));
	if (!strncmp(action, "Command", strlen("Command")))
	{
		ast_copy_string(command, astman_get_header(m, "Command"),
			sizeof(command));
		ast_log( LOG_DEBUG, "Manager received command '%s' '%s'\n", action,
			command );
	}
	else
		ast_log( LOG_DEBUG, "Manager received command '%s'\n", action );

	if (ast_strlen_zero(action)) {
		astman_send_error(s, m, "Missing action in request");
		return 0;
	}
        if (!ast_strlen_zero(id)) {
                snprintf(idText,256,"ActionID: %s\r\n",id);
        }
	if (!s->authenticated) {
		if (!strcasecmp(action, "Challenge")) {
			char *authtype;
			authtype = astman_get_header(m, "AuthType");
			if (!strcasecmp(authtype, "MD5")) {
				if (ast_strlen_zero(s->challenge))
					snprintf(s->challenge, sizeof(s->challenge), "%d", rand());
				ast_mutex_lock(&s->__lock);
				ast_cli(s->fd, "Response: Success\r\n"
						"%s"
						"Challenge: %s\r\n\r\n",
						idText,s->challenge);
				ast_mutex_unlock(&s->__lock);
				return 0;
			} else {
				astman_send_error(s, m, "Must specify AuthType");
				return 0;
			}
		} else if (!strcasecmp(action, "Login")) {
			if (authenticate(s, m)) {
				sleep(1);
				astman_send_error(s, m, "Authentication failed");
				return -1;
			} else {
				s->authenticated = 1;
				if (option_verbose > 1) {
					if ( displayconnects ) {
						ast_verbose(VERBOSE_PREFIX_2 "Manager '%s' logged on from %s\n", s->username, ast_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
					}
				}
				ast_log(LOG_EVENT, "Manager '%s' logged on from %s\n", s->username, ast_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
				astman_send_ack(s, m, "Authentication accepted");
			}
		} else if (!strcasecmp(action, "Logoff")) {
			astman_send_ack(s, m, "See ya");
			return -1;
		} else
			astman_send_error(s, m, "Authentication Required");
	} else {
		int ret=0;
		struct eventqent *eqe;
		ast_mutex_lock(&s->__lock);
		s->busy = 1;
		ast_mutex_unlock(&s->__lock);
		while( tmp ) { 		
			if (!strcasecmp(action, tmp->action)) {
				if ((s->writeperm & tmp->authority) == tmp->authority) {
					if (tmp->func(s, m))
						ret = -1;
				} else {
					astman_send_error(s, m, "Permission denied");
				}
				break;
			}
			tmp = tmp->next;
		}
		if (!tmp)
			astman_send_error(s, m, "Invalid/unknown command");
		ast_mutex_lock(&s->__lock);
		s->busy = 0;
		while(s->eventq) {
			if (ast_carefulwrite(s->fd, s->eventq->eventdata, strlen(s->eventq->eventdata), s->writetimeout) < 0) {
				ret = -1;
				break;
			}
			eqe = s->eventq;
			s->eventq = s->eventq->next;
			free(eqe);
		}
		ast_mutex_unlock(&s->__lock);
		return ret;
	}
	return 0;
}

static int get_input(struct mansession *s, char *output)
{
	/* output must have at least sizeof(s->inbuf) space */
	int res;
	int x;
	struct pollfd fds[1];
	char iabuf[INET_ADDRSTRLEN];
	for (x=1;x<s->inlen;x++) {
		if ((s->inbuf[x] == '\n') && (s->inbuf[x-1] == '\r')) {
			/* Copy output data up to and including \r\n */
			memcpy(output, s->inbuf, x + 1);
			/* Add trailing \0 */
			output[x+1] = '\0';
			/* Move remaining data back to the front */
			memmove(s->inbuf, s->inbuf + x + 1, s->inlen - x);
			s->inlen -= (x + 1);
			return 1;
		}
	} 
	if (s->inlen >= sizeof(s->inbuf) - 1) {
		ast_log(LOG_WARNING, "Dumping long line with no return from %s: %s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr), s->inbuf);
		s->inlen = 0;
	}
	fds[0].fd = s->fd;
	fds[0].events = POLLIN;
	do {
		res = poll(fds, 1, -1);
		if (res < 0) {
			if (errno == EINTR) {
				if (s->dead)
					return -1;
				continue;
			}
			ast_log(LOG_WARNING, "Select returned error: %s\n", strerror(errno));
	 		return -1;
		} else if (res > 0) {
			ast_mutex_lock(&s->__lock);
			res = read(s->fd, s->inbuf + s->inlen, sizeof(s->inbuf) - 1 - s->inlen);
			ast_mutex_unlock(&s->__lock);
			if (res < 1)
				return -1;
			break;
		}
	} while(1);
	s->inlen += res;
	s->inbuf[s->inlen] = '\0';
	return 0;
}

static void *session_do(void *data)
{
	struct mansession *s = data;
	struct message *m;
	char iabuf[INET_ADDRSTRLEN];
	int res;
	
	ast_mutex_lock(&s->__lock);
	ast_cli(s->fd, "Asterisk Call Manager/1.0\r\n");
	ast_mutex_unlock(&s->__lock);
	if (!(m = calloc(1,sizeof(struct message)))) {
		ast_log(LOG_ERROR, "Manager '%s' failed to allocate message\n",
			s->username);
		return NULL;
	}
	for (;;) {
		res = get_input(s, m->headers[m->hdrcount]);
		if (res > 0) {
			/* Strip trailing \r\n */
			if (strlen(m->headers[m->hdrcount]) < 2)
				continue;
			m->headers[m->hdrcount][strlen(m->headers[m->hdrcount]) - 2] = '\0';
			if (ast_strlen_zero(m->headers[m->hdrcount])) {
				if (process_message(s, m))
					break;			   
				memset(m, 0, sizeof(struct message));
			} else if (m->hdrcount < MAX_HEADERS - 1)
				m->hdrcount++;
		} else if (res < 0)
			break;
	}
	if (s->authenticated) {
		if (option_verbose > 1) {
			if (displayconnects) 
				ast_verbose(VERBOSE_PREFIX_2 "Manager '%s' logged off from %s\n", s->username, ast_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));    
		}
		ast_log(LOG_EVENT, "Manager '%s' logged off from %s\n", s->username, ast_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
	} else {
		if (option_verbose > 1) {
			if ( displayconnects )
				ast_verbose(VERBOSE_PREFIX_2 "Connect attempt from '%s' unable to authenticate\n", ast_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
		}
		ast_log(LOG_EVENT, "Failed attempt from %s\n", ast_inet_ntoa(iabuf, sizeof(iabuf), s->sin.sin_addr));
	}
	destroy_session(s);
	free(m);
	return NULL;
}

static void *accept_thread(void *ignore)
{
	int as;
	struct sockaddr_in sin;
	socklen_t sinlen;
	struct mansession *s;
	struct protoent *p;
	int arg = 1;
	int flags;
	pthread_attr_t attr;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	for (;;) {
		sinlen = sizeof(sin);
		as = accept(asock, (struct sockaddr *)&sin, &sinlen);
		if (as < 0) {
			ast_log(LOG_NOTICE, "Accept returned -1: %s\n", strerror(errno));
			continue;
		}
		p = getprotobyname("tcp");
		if (p) {
			if( setsockopt(as, p->p_proto, TCP_NODELAY, (char *)&arg, sizeof(arg) ) < 0 ) {
				ast_log(LOG_WARNING, "Failed to set manager tcp connection to TCP_NODELAY mode: %s\n", strerror(errno));
			}
		}
		s = malloc(sizeof(struct mansession));
		if (!s) {
			ast_log(LOG_WARNING, "Failed to allocate management session: %s\n", strerror(errno));
			continue;
		} 
		memset(s, 0, sizeof(struct mansession));
		memcpy(&s->sin, &sin, sizeof(sin));
		s->writetimeout = 100;

		if(! block_sockets) {
			/* For safety, make sure socket is non-blocking */
			flags = fcntl(as, F_GETFL);
			fcntl(as, F_SETFL, flags | O_NONBLOCK);
		}
		ast_mutex_init(&s->__lock);
		s->fd = as;
		s->send_events = -1;
		ast_mutex_lock(&sessionlock);
		s->next = sessions;
		sessions = s;
		ast_mutex_unlock(&sessionlock);
		if (ast_pthread_create(&s->t, &attr, session_do, s))
			destroy_session(s);
	}
	pthread_attr_destroy(&attr);
	return NULL;
}

static int append_event(struct mansession *s, const char *str)
{
	struct eventqent *tmp, *prev=NULL;
	tmp = malloc(sizeof(struct eventqent) + strlen(str));
	if (tmp) {
		tmp->next = NULL;
		strcpy(tmp->eventdata, str);
		if (s->eventq) {
			prev = s->eventq;
			while(prev->next) 
				prev = prev->next;
			prev->next = tmp;
		} else {
			s->eventq = tmp;
		}
		return 0;
	}
	return -1;
}

/*! \brief  manager_event: Send AMI event to client */
int manager_event(int category, char *event, char *fmt, ...)
{
	struct mansession *s;
	char auth[80];
	char tmp[4096] = "";
	char *tmp_next = tmp;
	size_t tmp_left = sizeof(tmp) - 2;
	va_list ap;

	ast_mutex_lock(&sessionlock);
	for (s = sessions; s; s = s->next) {
		if ((s->readperm & category) != category)
			continue;

		if ((s->send_events & category) != category)
			continue;

		if (ast_strlen_zero(tmp)) {
			ast_build_string(&tmp_next, &tmp_left, "Event: %s\r\nPrivilege: %s\r\n",
					 event, authority_to_str(category, auth, sizeof(auth)));
			va_start(ap, fmt);
			ast_build_string_va(&tmp_next, &tmp_left, fmt, ap);
			va_end(ap);
			*tmp_next++ = '\r';
			*tmp_next++ = '\n';
			*tmp_next = '\0';
		}

		ast_mutex_lock(&s->__lock);
		if (s->busy) {
			append_event(s, tmp);
		} else if (!s->dead) {
			if (ast_carefulwrite(s->fd, tmp, tmp_next - tmp, s->writetimeout) < 0) {
				ast_log(LOG_WARNING, "Disconnecting slow (or gone) manager session!\n");
				s->dead = 1;
				pthread_kill(s->t, SIGURG);
			}
		}
		ast_mutex_unlock(&s->__lock);
	}
	ast_mutex_unlock(&sessionlock);

	return 0;
}

/* Sends a response for a service dial */
static int service_dial_send_response(char *trunk, char *action_id, int status)
{
	return manager_event(EVENT_FLAG_CALL, "ServiceDialResp",
		"Status: %s\r\n"
		"Trunk: %s\r\n"
		"ActionID: %s\r\n", 
		status? "Success" : "Failed", trunk, action_id);
}

/* completion thread for the service call */
static void *service_dial_completion_thread(void *data)
{
	struct service_dial_data *sc_data =
		(struct service_dial_data*)data;
	struct ast_channel *chan = NULL; 
	struct ast_codec_pref slin;
	int outstate;
	char dn[256];

	/* prepare codec list */
	ast_codec_pref_init(&slin);
	ast_codec_pref_append_missing2(&slin, NATIVE_FORMATS);

	/* create SIP channel and dial to service number from the configured trunk */
	snprintf(dn, sizeof(dn), "%s/%s", sc_data->trunk, sc_data->exten);
	chan = __ast_request_and_dial("SIP", &slin, dn, sc_data->timeout, &outstate,
		sc_data->trunk, sc_data->trunk, NULL);

	/* if no channel created, send failed response */
	if (!chan)
	{
		ast_log(LOG_DEBUG, "Unable to create SIP channel or timeout expired"
			" (reason %d - %s)\n", outstate, ast_state2str(outstate,0));
		service_dial_send_response(sc_data->trunk, sc_data->action_id, 0);
		goto Exit;
	}

	if (chan->_state == AST_STATE_UP) 
	{
		ast_log(LOG_DEBUG, "Service call answered - can hangup and report"
			" SUCCESS\n");
		service_dial_send_response(sc_data->trunk, sc_data->action_id, 1);
	}
	else
	{
		ast_log(LOG_DEBUG, "Service call Failed -  hangup and report Failure\n");
		service_dial_send_response(sc_data->trunk, sc_data->action_id, 0);
	}

	ast_hangup(chan);
Exit:	
	free(data);
	return NULL;
}

static const char *diag_outcall_res2str(int result)
{
	int i;
	const struct diag_res {
	    int result;
	    const char *desc;
	} res2str[] = {
	    { DIAGNOSTICS_SUCCESS, "Success"},
	    { DIAGNOSTICS_TIMEDOUT, "TimeOut"},
	    { DIAGNOSTICS_CANCELED, "Canceled"},
	    { DIAGNOSTICS_FAILED, "Failed"},
	    { DIAGNOSTICS_ERROR, "Error"}
	};

	for (i = 0; i < sizeof(res2str) / sizeof(res2str[0]); i++)
	{
		if (res2str[i].result == result)
		    	return res2str[i].desc;
	}

	return "Undefined";
}

/* Sends a response for a service dial */
static int diag_outcall_send_response(char *action_id, int result)
{
	return manager_event(EVENT_FLAG_CALL, "DiagnosticResult",
		"Result: %s\r\n"
		"ActionID: %s\r\n", 
		diag_outcall_res2str(result), action_id);
}

static diagnostics_result do_diagnostics_outcall(char *exten, char *trunk_user,
	int timeout, int require_answer)
{
	int cause = 0;
	struct ast_channel *chan;
	struct ast_frame *f;
	int res = 0;
	struct ast_codec_pref formats;
	char dn[256];
	diagnostics_result diag_res = DIAGNOSTICS_FAILED;

	/* prepare codec list */
	ast_codec_pref_init(&formats);
	ast_codec_pref_append_missing2(&formats, NATIVE_FORMATS);
	
	/* set chan prop */
	snprintf(dn, sizeof(dn), "%s/%s",trunk_user, exten);

	chan = ast_request("SIP", &formats, dn, &cause);
	if (chan) 
	{
		ast_set_callerid(chan, trunk_user, trunk_user, trunk_user);
		/* set special diagnostic variable on this call. it will assist us to
		 * find this call if we'll want to cancel it */
		pbx_builtin_setvar_helper(chan, "DIAGNOSTIC_CALL", exten);
	
		/* start call */
		if (!ast_call(chan, dn, 0)) 
		{
		        diag_res = DIAGNOSTICS_IN_PROGRESS;
			while (timeout && diag_res == DIAGNOSTICS_IN_PROGRESS)
			{
				res = ast_waitfor(chan, timeout);
				if (res <= 0) {
					/* Something not cool, or timed out */
					break;
				}
				if (timeout > -1)
					timeout = res;
				f = ast_read(chan);
				if (!f) 
				{
				        char *val = pbx_builtin_getvar_helper(chan, "DIAGNOSTIC_CALL");

					diag_res = !strcmp(val, "Canceled") ? DIAGNOSTICS_CANCELED : DIAGNOSTICS_FAILED;

					break;
				} else if (f->frametype == AST_FRAME_CONTROL)
				{
				    	switch(f->subclass)
					{
					case AST_CONTROL_RINGING:
					case AST_CONTROL_PROGRESS:
						if (require_answer) 
							break;
						/* FALLTHROUGH */
					case AST_CONTROL_ANSWER:
						diag_res = DIAGNOSTICS_SUCCESS;
						break;
					case AST_CONTROL_BUSY:
					case AST_CONTROL_DECLINE:
					case AST_CONTROL_CONGESTION:
					case AST_CONTROL_UNALLOCATED:
						diag_res = DIAGNOSTICS_FAILED;
						break;
					}
				}
				ast_frfree(f);
			}

			if (!timeout || res <= 0)
				diag_res = DIAGNOSTICS_TIMEDOUT;

			ast_log(LOG_DEBUG, "Diagnostic Test completed - %s\n", diag_outcall_res2str(diag_res));

			/* this call should not have cdr */
			if (chan->cdr) 
			{
				ast_cdr_free(chan->cdr);
				chan->cdr = NULL;
			}
						
			ast_log(LOG_DEBUG, "Hangup the diagnostic call\n");
			ast_hangup(chan);
			chan = NULL;
		}
		else
			ast_log(LOG_NOTICE, "Unable to start diagnostic call channel SIP/%s\n", dn);
	} else 
		ast_log(LOG_NOTICE, "Unable to request channel SIP/%s\n",  dn);

	return diag_res;
}

/* completion thread for the diagnostic outgoing call */
static void *diag_outcall_completion_thread(void *data)
{
	diagnostics_result res;
	struct diag_outcall_data *outcall_data =
		(struct diag_outcall_data*)data;

	res = do_diagnostics_outcall(outcall_data->exten, outcall_data->trunk,
		outcall_data->timeout, outcall_data->require_answer);

	diag_outcall_send_response(outcall_data->action_id, res);

	free(data);
	return NULL;
}

static char mandescr_diag_outcall_start[] = 
"Description: Generates a silent diagnostic outgoing call\n";

static int action_diag_outcall_start(struct mansession *s, struct message *m)
{
	int res;
	pthread_attr_t attr;
	struct diag_outcall_data *outcall_data = NULL;
	char *exten = astman_get_header(m, "Number");
	char *trunk_name = astman_get_header(m, "Trunk");
	int dial_timeout = atoi(astman_get_header(m, "Timeout")) * 1000;
	int require_answer = atoi(astman_get_header(m, "Answer"));

	ast_log(LOG_DEBUG, "Need to start diagnostic call from trunk %s to %s with"
		" timeout %d (require answer %d)\n", trunk_name, exten, dial_timeout, 
		require_answer);

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    diag_outcall_thread = AST_PTHREADT_NULL;

	/* verify received arguments */
	if (!exten || !trunk_name)
	{
		diag_outcall_send_response(astman_get_header(m, "ActionID"), 
			DIAGNOSTICS_ERROR);
		goto Exit;
	}

	/* allocate data memory for the completion thread */
	outcall_data = (struct diag_outcall_data*) calloc(1, sizeof(struct
		diag_outcall_data));
	
	if (!outcall_data)
	{
		ast_log(LOG_DEBUG, "cant allocate memory for Diagnostics Outgoing call"
			" - Out of memory!\n");
		diag_outcall_send_response(astman_get_header(m, "ActionID"),
			DIAGNOSTICS_ERROR);
		goto Exit;
	}

	outcall_data->timeout = dial_timeout;
	outcall_data->require_answer = require_answer;
	strncpy(outcall_data->trunk, trunk_name, sizeof(outcall_data->trunk));
	strncpy(outcall_data->exten, exten, sizeof(outcall_data->exten));
	strncpy(outcall_data->action_id, astman_get_header(m, "ActionID"), 
		sizeof(outcall_data->action_id));

	/* create completion thread */
	res = ast_pthread_create(&diag_outcall_thread, &attr,
		diag_outcall_completion_thread, (void*)outcall_data);
	if (res)
	{
	    ast_log(LOG_ERROR, "Unable to run async diagnostics outgoing call! "
			"(res %d)\n", res);
		diag_outcall_send_response(astman_get_header(m, "ActionID"),
			DIAGNOSTICS_ERROR);
	}

Exit:
	pthread_attr_destroy(&attr);
	return 0;
}

static char mandescr_diag_outcall_cancel[] = 
"Description: Cancels an ongoing diagnostic outgoing call\n";

static int action_diag_outcall_cancel(struct mansession *s, struct message *m)
{
	struct ast_channel *chan = NULL;
	
	/* find the diagnostic channel */
	while ((chan = ast_channel_walk_locked(chan)) &&
		!pbx_builtin_getvar_helper(chan, "DIAGNOSTIC_CALL"))
	{
		ast_mutex_unlock(&chan->lock);
	}

	/* if channel was found, hangup the call */
	if (chan)
	{
		ast_mutex_unlock(&chan->lock);
		ast_log(LOG_DEBUG, "found diagnostic channel %s. terminate it!\n", chan->name);
		if (ast_queue_hangup(chan))
		    return -1;
		pbx_builtin_setvar_helper(chan, "DIAGNOSTIC_CALL", "Canceled");
	}
	return 0;
}

static char mandescr_service_dial[] = 
"Description: Generates a silent outgoing call to service center\n";

static int action_service_dial(struct mansession *s, struct message *m)
{
	int res;
	pthread_attr_t attr;
	struct service_dial_data* sc_data = NULL;
	char *exten = astman_get_header(m, "Exten");
	char *trunk_name = astman_get_header(m, "Trunk");
	int dial_timeout = atoi(astman_get_header(m, "Timeout")) * 1000;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    service_dial_thread = AST_PTHREADT_NULL;

	/* verify received arguments */
	if (!exten || !trunk_name || !dial_timeout)
	{
		astman_send_error(s, m, "Some arguments not specified");
		goto Exit;
	}

	/* allocate data memory for the completion thread */
	sc_data = (struct service_dial_data*) calloc(1, sizeof(struct
		service_dial_data));
	
	if (!sc_data)
	{
		ast_log(LOG_DEBUG, "cant allocate memory for service dial - Out of memory!\n");
		astman_send_error(s, m, "Out of memory for silent call\n");
		service_dial_send_response(trunk_name, astman_get_header(m, "ActionID"), 0);
		goto Exit;
	}	

	sc_data->timeout = dial_timeout;
	strncpy(sc_data->trunk, trunk_name, sizeof(sc_data->trunk));
	strncpy(sc_data->exten, exten, sizeof(sc_data->exten));
	strncpy(sc_data->action_id, astman_get_header(m, "ActionID"), sizeof(sc_data->action_id));

	/* create completion thread */
	res = ast_pthread_create(&service_dial_thread, &attr,
		service_dial_completion_thread, (void*)sc_data);
	if (res)
	{
	    ast_log(LOG_ERROR, "Unable to run async service call! (res %d)\n", res);
	    astman_send_error(s, m, "Failed to establish service call\n");
		service_dial_send_response(trunk_name, astman_get_header(m, "ActionID"), 0);
	}

Exit:
	pthread_attr_destroy(&attr);
	return 0;
}

static char mandescr_system_time_updated[] = 
"Description: Update all stored timestamps because of updated system time\n";

static int action_system_time_updated(struct mansession *s, struct message *m)
{
	struct ast_channel *chan = NULL;
	time_t old_time = atoi(astman_get_header(m, "OldTime"));
	time_t new_time = atoi(astman_get_header(m, "NewTime"));

	ast_log(LOG_EVENT, "System time updated from %d to %d", (int)old_time, (int)new_time);

	while ((chan = ast_channel_walk_locked(chan)))
	{
		if (chan->cdr)
		{
			ast_log(LOG_DEBUG, "Channel: %s\nCDR: start.tv_sec = %d\nanswer.tv_sec = %d\nend.tv_sec = %d\nduration = %d\nbillsec = %d",
				chan->name, (int)chan->cdr->start.tv_sec, (int)chan->cdr->answer.tv_sec, (int)chan->cdr->end.tv_sec, (int)chan->cdr->duration, (int)chan->cdr->billsec);
			if (chan->cdr->start.tv_sec)
				chan->cdr->start.tv_sec += new_time - old_time;
			if (chan->cdr->answer.tv_sec)
				chan->cdr->answer.tv_sec += new_time - old_time;
		}
		ast_mutex_unlock(&chan->lock);
	}

	return 0;
}

int ast_manager_unregister( char *action ) 
{
	struct manager_action *cur = first_action, *prev = first_action;

	ast_mutex_lock(&actionlock);
	while( cur ) { 		
		if (!strcasecmp(action, cur->action)) {
			prev->next = cur->next;
			free(cur);
			if (option_verbose > 1) 
				ast_verbose(VERBOSE_PREFIX_2 "Manager unregistered action %s\n", action);
			ast_mutex_unlock(&actionlock);
			return 0;
		}
		prev = cur;
		cur = cur->next;
	}
	ast_mutex_unlock(&actionlock);
	return 0;
}

static int manager_state_cb(char *context, char *exten, int state, void *data)
{
	/* Notify managers of change */
	manager_event(EVENT_FLAG_CALL, "ExtensionStatus", "Exten: %s\r\nContext: %s\r\nStatus: %d\r\n", exten, context, state);
	return 0;
}

static int ast_manager_register_struct(struct manager_action *act)
{
	struct manager_action *cur = first_action, *prev = NULL;
	int ret;

	ast_mutex_lock(&actionlock);
	while(cur) { /* Walk the list of actions */
		ret = strcasecmp(cur->action, act->action);
		if (ret == 0) {
			ast_log(LOG_WARNING, "Manager: Action '%s' already registered\n", act->action);
			ast_mutex_unlock(&actionlock);
			return -1;
		} else if (ret > 0) {
			/* Insert these alphabetically */
			if (prev) {
				act->next = prev->next;
				prev->next = act;
			} else {
				act->next = first_action;
				first_action = act;
			}
			break;
		}
		prev = cur; 
		cur = cur->next;
	}
	
	if (!cur) {
		if (prev)
			prev->next = act;
		else
			first_action = act;
		act->next = NULL;
	}

	if (option_verbose > 1) 
		ast_verbose(VERBOSE_PREFIX_2 "Manager registered action %s\n", act->action);
	ast_mutex_unlock(&actionlock);
	return 0;
}

/*! \brief register a new command with manager, including online help. This is 
	the preferred way to register a manager command */
int ast_manager_register2(const char *action, int auth, int (*func)(struct mansession *s, struct message *m), const char *synopsis, const char *description)
{
	struct manager_action *cur;

	cur = malloc(sizeof(struct manager_action));
	if (!cur) {
		ast_log(LOG_WARNING, "Manager: out of memory trying to register action\n");
		return -1;
	}
	cur->action = action;
	cur->authority = auth;
	cur->func = func;
	cur->synopsis = synopsis;
	cur->description = description;
	cur->next = NULL;

	ast_manager_register_struct(cur);

	return 0;
}
/*! @}
 END Doxygen group */

static int registered = 0;

int init_manager(void)
{
	struct ast_config *cfg;
	char *val;
	int oldportno = portno;
	static struct sockaddr_in ba;
	int x = 1;
	if (!registered) {
		/* Register default actions */
		ast_manager_register2("Ping", 0, action_ping, "Keepalive command", mandescr_ping);
		ast_manager_register2("Events", 0, action_events, "Control Event Flow", mandescr_events);
		ast_manager_register2("Logoff", 0, action_logoff, "Logoff Manager", mandescr_logoff);
		ast_manager_register2("Hangup", EVENT_FLAG_CALL, action_hangup, "Hangup Channel", mandescr_hangup);
		ast_manager_register("Status", EVENT_FLAG_CALL, action_status, "Lists channel status" );
		ast_manager_register2("Setvar", EVENT_FLAG_CALL, action_setvar, "Set Channel Variable", mandescr_setvar );
		ast_manager_register2("Getvar", EVENT_FLAG_CALL, action_getvar, "Gets a Channel Variable", mandescr_getvar );
		ast_manager_register2("Redirect", EVENT_FLAG_CALL, action_redirect, "Redirect (transfer) a call", mandescr_redirect );
		ast_manager_register2("Originate", EVENT_FLAG_CALL, action_originate, "Originate Call", mandescr_originate);
		ast_manager_register2("ServiceDial", EVENT_FLAG_CALL,
			action_service_dial, "Service Dial", mandescr_service_dial);
		ast_manager_register2("StartDiagnostic", EVENT_FLAG_CALL, action_diag_outcall_start, "Start Diagnostic outgoing call", mandescr_diag_outcall_start);
		ast_manager_register2("CancelDiagnostic", EVENT_FLAG_CALL, action_diag_outcall_cancel, "Cancel Diagnostic outgoing call", mandescr_diag_outcall_cancel);
		ast_manager_register2("Command", EVENT_FLAG_COMMAND, action_command, "Execute Asterisk CLI Command", mandescr_command );
		ast_manager_register2("ExtensionState", EVENT_FLAG_CALL, action_extensionstate, "Check Extension Status", mandescr_extensionstate );
		ast_manager_register2("AbsoluteTimeout", EVENT_FLAG_CALL, action_timeout, "Set Absolute Timeout", mandescr_timeout );
		ast_manager_register2("MailboxStatus", EVENT_FLAG_CALL, action_mailboxstatus, "Check Mailbox", mandescr_mailboxstatus );
		ast_manager_register2("MailboxCount", EVENT_FLAG_CALL, action_mailboxcount, "Check Mailbox Message Count", mandescr_mailboxcount );
		ast_manager_register2("ListCommands", 0, action_listcommands, "List available manager commands", mandescr_listcommands);
		ast_manager_register2("TimeUpdate", EVENT_FLAG_CALL, action_system_time_updated, "Update timestamps", mandescr_system_time_updated);
		ast_manager_register2("OriginateCancel", EVENT_FLAG_CALL,
		    action_originate_cancel, "Cancel Originate call",
		    mandescr_originate_cancel);
		ast_cli_register(&show_mancmd_cli);
		ast_cli_register(&show_mancmds_cli);
		ast_cli_register(&show_manconn_cli);
		ast_extension_state_add(NULL, NULL, manager_state_cb, NULL);
		registered = 1;
	}
	portno = DEFAULT_MANAGER_PORT;
	displayconnects = 1;
	cfg = ast_config_load("manager.conf");
	if (!cfg) {
		ast_log(LOG_NOTICE, "Unable to open management configuration manager.conf.  Call management disabled.\n");
		return 0;
	}
	memset(&ba, 0, sizeof(ba));
	val = ast_variable_retrieve(cfg, "general", "enabled");
	if (val)
		enabled = ast_true(val);

	val = ast_variable_retrieve(cfg, "general", "block-sockets");
	if(val)
		block_sockets = ast_true(val);

	if ((val = ast_variable_retrieve(cfg, "general", "port"))) {
		if (sscanf(val, "%d", &portno) != 1) {
			ast_log(LOG_WARNING, "Invalid port number '%s'\n", val);
			portno = DEFAULT_MANAGER_PORT;
		}
	} else if ((val = ast_variable_retrieve(cfg, "general", "portno"))) {
		if (sscanf(val, "%d", &portno) != 1) {
			ast_log(LOG_WARNING, "Invalid port number '%s'\n", val);
			portno = DEFAULT_MANAGER_PORT;
		}
		ast_log(LOG_NOTICE, "Use of portno in manager.conf deprecated.  Please use 'port=%s' instead.\n", val);
	}
	/* Parsing the displayconnects */
	if ((val = ast_variable_retrieve(cfg, "general", "displayconnects"))) {
			displayconnects = ast_true(val);;
	}
				
	
	ba.sin_family = AF_INET;
	ba.sin_port = htons(portno);
	memset(&ba.sin_addr, 0, sizeof(ba.sin_addr));
	
	if ((val = ast_variable_retrieve(cfg, "general", "bindaddr"))) {
		if (!inet_aton(val, &ba.sin_addr)) { 
			ast_log(LOG_WARNING, "Invalid address '%s' specified, using 0.0.0.0\n", val);
			memset(&ba.sin_addr, 0, sizeof(ba.sin_addr));
		}
	}
	
	if ((asock > -1) && ((portno != oldportno) || !enabled)) {
#if 0
		/* Can't be done yet */
		close(asock);
		asock = -1;
#else
		ast_log(LOG_WARNING, "Unable to change management port / enabled\n");
#endif
	}
	ast_config_destroy(cfg);
	
	/* If not enabled, do nothing */
	if (!enabled) {
		return 0;
	}
	if (asock < 0) {
		asock = socket(AF_INET, SOCK_STREAM, 0);
		if (asock < 0) {
			ast_log(LOG_WARNING, "Unable to create socket: %s\n", strerror(errno));
			return -1;
		}
		setsockopt(asock, SOL_SOCKET, SO_REUSEADDR, &x, sizeof(x));
		if (bind(asock, (struct sockaddr *)&ba, sizeof(ba))) {
			ast_log(LOG_WARNING, "Unable to bind socket: %s\n", strerror(errno));
			close(asock);
			asock = -1;
			return -1;
		}
		if (listen(asock, 2)) {
			ast_log(LOG_WARNING, "Unable to listen on socket: %s\n", strerror(errno));
			close(asock);
			asock = -1;
			return -1;
		}
		if (option_verbose)
			ast_verbose("Asterisk Management interface listening on port %d\n", portno);
		ast_pthread_create(&t, NULL, accept_thread, NULL);
	}
	return 0;
}

int reload_manager(int check_conf_file)
{
	manager_event(EVENT_FLAG_SYSTEM, "Reload", "Message: Reload Requested\r\n");

	if (!ast_config_file_md5_update("manager.conf", conf_file_md5) && check_conf_file)
	{
		ast_log(LOG_DEBUG, "Skipping reload since manager.conf was not changed\n");
		return 0;
	}

	return init_manager();
}
