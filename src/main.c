/* $Id: main.c,v 1.45 2003/04/16 11:10:17 jajcus Exp $ */

/*
 *  (C) Copyright 2002 Jacek Konieczny <jajcus@pld.org.pl>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License Version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "ggtrans.h"
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <limits.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <ctype.h>
#include <time.h>
#include <locale.h>
#include "jabber.h"
#include "sessions.h"
#include "encoding.h"
#include "requests.h"
#include "conf.h"
#include "acl.h"
#include "debug.h"

#ifndef OPEN_MAX
#define OPEN_MAX 255
#endif

GMainLoop *main_loop;

static int signal_received=FALSE;
static gboolean the_end=FALSE;
gboolean do_restart=FALSE;
static int restart_timeout=60;
static gboolean foreground=TRUE;
static int debug_level=0;

static FILE *log_file=NULL;
static gboolean use_syslog=FALSE;
static char *pid_filename=NULL;
static GAllocator* list_allocator;

GList *admins=NULL;
time_t start_time=0;
unsigned long packets_in=0;
unsigned long packets_out=0;
unsigned long gg_messages_in=0;
unsigned long gg_messages_out=0;

static struct {
	const char *name;
	int code;
}facilitynames[] =
  {
    { "auth", LOG_AUTH },
    { "authpriv", LOG_AUTHPRIV },
    { "cron", LOG_CRON },
    { "daemon", LOG_DAEMON },
    { "ftp", LOG_FTP },
    { "kern", LOG_KERN },
    { "lpr", LOG_LPR },
    { "mail", LOG_MAIL },
    { "news", LOG_NEWS },
    { "syslog", LOG_SYSLOG },
    { "user", LOG_USER },
    { "uucp", LOG_UUCP },
    { "local0", LOG_LOCAL0 },
    { "local1", LOG_LOCAL1 },
    { "local2", LOG_LOCAL2 },
    { "local3", LOG_LOCAL3 },
    { "local4", LOG_LOCAL4 },
    { "local5", LOG_LOCAL5 },
    { "local6", LOG_LOCAL6 },
    { "local7", LOG_LOCAL7 },
    { NULL, -1 }
  };


void sigchld_handler (int signum){
int pid, status, serrno;
	serrno = errno;
	while (1){
		   pid = waitpid (WAIT_ANY, &status, WNOHANG);
		   if (pid<=0)
		     break;
	}
	errno = serrno;
}


void signal_handler(int sig){

	switch(sig){
		case SIGHUP:
			restart_timeout=0;
			do_restart=TRUE;
		case SIGINT:
		case SIGTERM:
			the_end=TRUE;
			break;
		case SIGPIPE:
			break;
		default:
			break;
	}
	signal(sig,signal_handler);
	signal_received=sig;
}

gboolean signal_source_prepare(gpointer  source_data,
				GTimeVal *current_time,
				gint	 *timeout,
				gpointer  user_data){

	*timeout=1000;
	if (signal_received) return TRUE;
	return FALSE;
}

gboolean signal_source_check(gpointer  source_data,
				GTimeVal *current_time,
				gpointer  user_data){

	if (signal_received) return TRUE;
	return FALSE;
}

gboolean signal_source_dispatch(gpointer  source_data,
			GTimeVal *current_time,
			gpointer  user_data){

	psignal(signal_received,"signal received");
	g_warning("Signal received: %s",g_strsignal(signal_received));
	if (the_end) g_main_quit(main_loop);
	if (signal_received==SIGUSR1){
		g_message("Active sessions:");
		sessions_print_all(1);
	}
	signal_received=0;
	return TRUE;
}

void signal_source_destroy(gpointer user_data){
}

static GSourceFuncs signal_source_funcs={
		signal_source_prepare,
		signal_source_check,
		signal_source_dispatch,
		signal_source_destroy
		};

void log_handler_file(FILE *f,const gchar *log_domain, GLogLevelFlags log_level,
			const gchar *message){

	if (log_domain && log_domain[0]) fprintf(f,"%s: ",log_domain);
	switch(log_level){
		case G_LOG_LEVEL_ERROR:
			fprintf(f,_("Fatal error: %s\n"),_(message));
			break;
		case G_LOG_LEVEL_CRITICAL:
			fprintf(f,_("Error: %s\n"),_(message));
			break;
		case G_LOG_LEVEL_WARNING:
			fprintf(f,_("Warning: %s\n"),_(message));
			break;
		case G_LOG_LEVEL_MESSAGE:
			if (debug_level<-1) break;
		case G_LOG_LEVEL_INFO:
			if (debug_level<0) break;
			fprintf(f,"%s\n",_(message));
			break;
		case G_LOG_LEVEL_DEBUG:
			if (debug_level>0)
				fprintf(f,_("Debug: %s\n"),_(message));
			break;
		default:
			fprintf(f,_("Unknown: %s\n"),_(message));
			break;
	}
}

void log_handler_syslog(const gchar *log_domain, GLogLevelFlags log_level,
			const gchar *message){

	switch(log_level){
		case G_LOG_LEVEL_ERROR:
			syslog(LOG_ERR,_("Fatal error: %s"),_(message));
			break;
		case G_LOG_LEVEL_CRITICAL:
			syslog(LOG_ERR,_("Error: %s"),_(message));
			break;
		case G_LOG_LEVEL_WARNING:
			syslog(LOG_WARNING,_("Warning: %s"),_(message));
			break;
		case G_LOG_LEVEL_MESSAGE:
			if (debug_level<-1) break;
			syslog(LOG_NOTICE,"%s",_(message));
			break;
		case G_LOG_LEVEL_INFO:
			if (debug_level<0) break;
			syslog(LOG_NOTICE,"%s",_(message));
			break;
		case G_LOG_LEVEL_DEBUG:
			if (debug_level>0)
				syslog(LOG_DEBUG,_("Debug: %s\n"),_(message));
			break;
		default:
			syslog(LOG_NOTICE,_("Unknown: %s\n"),_(message));
			break;
	}
}

void log_handler(const gchar *log_domain, GLogLevelFlags log_level,
			const gchar *message, gpointer user_data){

char *lc_ctype,*lc_messages,*td_codeset;

	td_codeset=bind_textdomain_codeset(PACKAGE,NULL);
	lc_ctype=setlocale(LC_CTYPE,NULL);
	lc_messages=setlocale(LC_MESSAGES,NULL);

	log_level&=G_LOG_LEVEL_MASK;
	if (foreground) log_handler_file(stderr,log_domain,log_level,message);
	if (log_file) log_handler_file(log_file,log_domain,log_level,message);
	if (use_syslog) log_handler_syslog(log_domain,log_level,message);

	setlocale(LC_CTYPE,lc_ctype);
	setlocale(LC_MESSAGES,lc_ctype);
	bind_textdomain_codeset(PACKAGE,td_codeset);
}

void daemonize(FILE *pidfile){
pid_t pid;
pid_t sid;
int fd;

	debug(N_("Daemonizing..."));
	pid=fork();
	if (pid==-1) g_error(N_("Failed to fork(): %s"),g_strerror(errno));
	if (pid){
		if (pidfile){
			fprintf(pidfile,"%u",pid);
			fclose(pidfile);
		}
		debug(N_("Daemon born, pid %i."),pid);
		exit(0);
	}

	for (fd=0; fd < OPEN_MAX; fd++)
		close(fd);

	fd = open("/dev/null", O_RDWR);
	if (fd){
		if (fd != 0)
			dup2(fd, 0);
		if (fd != 1)
			dup2(fd, 1);
		if (fd != 2)
			dup2(fd, 2);
		if (fd > 2)
			close(fd);
	}

	sid=setsid();
	if (sid==-1) abort();
	foreground=FALSE;
	debug(N_("I am a daemon, I think."));
	return;
}

void usage(const char *name){
char *p;

	p=strrchr(name,'/');
	if (p) name=p+1;
	printf(_("\nJabber GaduGadu Transport %s\n"),VERSION);
	printf(_("\n"));
	printf(_("\tUsage: %s [OPTIONS]... [<config file>]\n"),name);
	printf(_("\nOptions:\n"));
	printf(_("\t-h	      This message\n"));
	printf(_("\t-f	      Run in foreground. Debug/error messages will be sent to stderr\n"));
	printf(_("\t-d <n>	Log level (0(default) - normal, >0 more, <0 less)\n"));
	printf(_("\t-D <n>	libgg debug level (enables also -f)\n"));
	printf(_("\t-u <user>	Switch to uid of <user> on startup\n"));
	printf(_("\t-g <group>	Switch to gid of <group> on startup\n"));
	printf(_("\n"));
}

int main(int argc,char *argv[]){
int c;
gboolean fg=FALSE;
xmlnode tag;
char *log_type=NULL;
char *log_filename=NULL;
char *str;
char *config_file;
int log_facility=-1;
uid_t uid,euid,newgid;
struct passwd *pwd;
struct group *grp;
char *user,*group;
char *data;
char saved_pwd_b[1024],*saved_pwd;
const char *param_d=NULL,*param_D=NULL;
int restarting=0;
FILE *pidfile;
guint lh;

	uid=getuid();
	euid=geteuid();
	if (euid==0 && uid!=euid){
		fprintf(stderr,"Refusing to work setuid-root!\n");
		exit(1);
	}
	newgid=0; user=NULL; group=NULL;

	/* use local locale for error and debug messages */
	setlocale(LC_MESSAGES,"");
	setlocale(LC_CTYPE,"");
	textdomain(PACKAGE);

	saved_pwd=getcwd(saved_pwd_b,1024);
	opterr=0;
	while ((c = getopt (argc, argv, "Rhfd:D:u:g:")) != -1){
		switch(c){
			case 'R':
				restarting=1;
				break;
			case 'h':
				usage(argv[0]);
				return 0;
			case 'f':
				fg=TRUE;
				break;
			case 'd':
				param_d=optarg;
				debug_level=atoi(optarg);
				break;
			case 'D':
				param_D=optarg;
				gg_debug_level=atoi(optarg);
				fg=TRUE;
				break;
			case 'u':
				if (uid!=0) g_error(_("Cannot change user."));
				user=optarg;
				break;
			case 'g':
				if (uid!=0) g_error(_("Cannot change group."));
				group=optarg;
				break;
			case '?':
				if (isprint(optopt))
					fprintf(stderr,_("Unknown command-line option: -%c.\n"),optopt);
				else
					fprintf(stderr,_("Unknown command-line option: -\\%03o.\n"),optopt);
				usage(argv[0]);
				return 1;
			default:
				g_error(_("Error while processing command line options"));
				break;
		}
	}

	if (optind<argc-1){
		fprintf(stderr,_("Unexpected argument: %s\n"),argv[optind]);
		usage(argv[0]);
		return 1;
	}

	if (optind==argc-1) config_file=g_strdup(argv[optind]);
	else config_file=g_strdup_printf("%s/%s",SYSCONFDIR,"jggtrans.xml");

	/* own allocator will be usefull for mem-leak tracing */
	list_allocator=g_allocator_new("la",128);
	g_list_push_allocator(list_allocator);

	lh=g_log_set_handler(NULL,G_LOG_FLAG_FATAL | G_LOG_LEVEL_ERROR
				| G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING
				| G_LOG_LEVEL_MESSAGE | G_LOG_LEVEL_INFO
				| G_LOG_LEVEL_DEBUG,log_handler,NULL);

	/* now the log handlers worry about the right language */
	setlocale(LC_MESSAGES,"C");
	setlocale(LC_CTYPE,"C");
	bind_textdomain_codeset(PACKAGE,"UTF-8");

	config=xmlnode_file(config_file);
	if (!config){
		g_error(N_("Couldn't load config!"));
		return 1;
	}
	str=xmlnode_get_name(config);
	if (!str || strcmp(str,"jggtrans")){
		g_error(N_("%s doesn't look like jggtrans config file."),config_file);
		return 1;
	}
	g_free(config_file);

	for(tag=xmlnode_get_firstchild(config);tag;tag=xmlnode_get_nextsibling(tag)){
		str=xmlnode_get_name(tag);
		if (!str) continue;
		if (!strcmp(str,"admin")){
			data=xmlnode_get_data(tag);
			admins=g_list_append(admins,data);
		}
		if (strcmp(str,"log")) continue;
		log_type=xmlnode_get_attrib(tag,"type");
		if (!strcmp(log_type,"syslog")){
			if (log_facility!=-1){
				g_warning(N_("Multiple syslog configs specified. Using only one."));
				continue;
			}
			str=xmlnode_get_attrib(tag,"facility");
			if (!str){
				log_facility=LOG_USER;
				continue;
			}
			for(log_facility=0;facilitynames[log_facility].name;log_facility++)
				if (!strcmp(facilitynames[log_facility].name,str)) break;
			if (!facilitynames[log_facility].name)
				 g_error(N_("Unknown syslog facility: %s"),str);
		}
		else if (!strcmp(log_type,"file")){
			if (log_filename) g_warning(N_("Multiple log files specified. Using only one."));
			else{
				data=xmlnode_get_data(tag);
				if (data!=NULL)
					log_filename=g_strstrip(data);
			}
		}
		else g_warning(N_("Ignoring unknown log type: %s"),xmlnode2str(tag));
	}

	pid_filename=config_load_string("pidfile");

	restart_timeout=config_load_int("restart_timeout",restart_timeout);

	if (pid_filename && !restarting){
		pidfile=fopen(pid_filename,"r");
		if (pidfile){
			pid_t pid;
			int r;

			r=fscanf(pidfile,"%u",&pid);
			fclose(pidfile);
			if (r==1 && pid>0){
				r=kill(pid,0);
				if (!r || (r && errno!=ESRCH)) g_error(N_("jggtrans already running"));
				if (r){
					g_warning(N_("Stale pid file. Removing."));
					unlink(pid_filename);
				}
			}
			else if (r!=EOF) g_error(N_("Invalid pid file."));
		}
		pidfile=fopen(pid_filename,"w");
		if (pidfile==NULL)
			g_error(N_("Couldn't open pidfile %s"),pid_filename);
	}
	else
		pidfile=NULL;

	if (group){
		grp=getgrnam(group);
		if (!grp) g_error(N_("Couldn't find group %s"),group);
		newgid=grp->gr_gid;
	}
	if (user){
		pwd=getpwnam(user);
		if (!pwd) g_error(N_("Couldn't find user %s"),user);
		if (newgid<=0) newgid=pwd->pw_gid;
		fchown(fileno(pidfile),pwd->pw_uid,newgid);
		if (setgid(newgid)) g_error(N_("Couldn't change group: %s"),g_strerror(errno));
		if (initgroups(user,newgid)) g_error(N_("Couldn't init groups: %s"),g_strerror(errno));
		if (setuid(pwd->pw_uid)) g_error(N_("Couldn't change user: %s"),g_strerror(errno));
	}
	else if (uid==0 && !restarting) g_error(N_("Refusing to run with uid=0"));

	main_loop=g_main_new(0);

	if (jabber_init()) return 1;
	if (sessions_init()) return 1;
	if (users_init()) return 1;
	if (encoding_init()) return 1;
	if (requests_init()) return 1;
	if (acl_init()) return 1;

	if (!fg && !restarting) daemonize(pidfile);
	else if (pidfile!=NULL){
		fprintf(pidfile,"%i",getpid());
		fclose(pidfile);
	}

	if (log_filename){
		log_file=fopen(log_filename,"a");
		if (!log_file) g_critical(N_("Couldn't open log file '%s': %s"),
						log_filename,g_strerror(errno));
		if (log_file) setvbuf(log_file,NULL,_IOLBF,0);
	}

	if (log_facility!=-1){
		openlog("jggtrans",0,log_facility);
		use_syslog=1;
	}

	if (jabber_connect()) return 1;

	g_source_add(G_PRIORITY_HIGH,0,&signal_source_funcs,NULL,NULL,NULL);
	signal(SIGPIPE,signal_handler);
	signal(SIGHUP,signal_handler);
	signal(SIGINT,signal_handler);
	signal(SIGTERM,signal_handler);
	signal(SIGUSR1,signal_handler);
	signal(SIGCHLD,sigchld_handler);

	start_time=time(NULL);
	g_main_run(main_loop);

	sessions_done();
	users_done();
	requests_done();
	jabber_done();
	encoding_done();
	acl_done();
	g_main_destroy(main_loop);

	if (do_restart && restart_timeout>=0){
		char *newargv[10];
		int n;

		g_message(N_("Restarting in %i seconds.\n"),restart_timeout);
		if (restart_timeout>0) sleep(restart_timeout);
		if (saved_pwd) chdir(saved_pwd);

		n=0;
		newargv[n++]=argv[0];
		newargv[n++]="-R";
		if (param_d){
			newargv[n++]="-d";
			newargv[n++]=(char *)param_d;
		}
		if (param_D){
			newargv[n++]="-D";
			newargv[n++]=(char *)param_D;
		}
		newargv[n]=NULL;
		if (!the_end){
			execvp(argv[0],newargv);
			perror("exec");
			return 1;
		}
	}

	g_message(N_("Exiting normally.\n"));

	g_log_remove_handler(NULL,lh);

	g_list_pop_allocator();
	g_allocator_free(list_allocator);

	if (log_file!=NULL){
		fclose(log_file);
		log_file=NULL;
	}

	if (pid_filename){
		if (unlink(pid_filename)!=0){
			pidfile=fopen(pid_filename,"w");
			if (pidfile) fclose(pidfile);
		}
	}
	xmlnode_free(config);

	return 0;
}

