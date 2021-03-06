/*
 * Asterisk -- An open source telephony toolkit.
 *
 * Copyright (C) 1999 - 2006, Digium, Inc.
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
 * \brief Comedian Mail - Voicemail System
 *
 * \author Mark Spencer <markster@digium.com>
 *
 * \extref Unixodbc - http://www.unixodbc.org
 * \extref A source distribution of University of Washington's IMAP
c-client (http://www.washington.edu/imap/
 * 
 * \par See also
 * \arg \ref Config_vm
 * \note For information about voicemail IMAP storage, read doc/imapstorage.txt
 * \ingroup applications
 * \note This module requires res_adsi to load. This needs to be optional
 * during compilation.
 *
 *
 *
 * \note  This file is now almost impossible to work with, due to all \#ifdefs.
 *        Feels like the database code before realtime. Someone - please come up
 *        with a plan to clean this up.
 */

/*** MODULEINFO
	<depend>res_smdi</depend>
 ***/

/*** MAKEOPTS
<category name="MENUSELECT_OPTS_app_voicemail" displayname="Voicemail Build Options" positive_output="yes" remove_on_change="apps/app_voicemail.o apps/app_directory.o">
	<member name="ODBC_STORAGE" displayname="Storage of Voicemail using ODBC">
		<depend>unixodbc</depend>
		<depend>ltdl</depend>
		<conflict>IMAP_STORAGE</conflict>
		<defaultenabled>no</defaultenabled>
	</member>
	<member name="IMAP_STORAGE" displayname="Storage of Voicemail using IMAP4">
		<depend>imap_tk</depend>
		<conflict>ODBC_STORAGE</conflict>
		<use>ssl</use>
		<defaultenabled>no</defaultenabled>
	</member>
</category>
 ***/

/* It is important to include the IMAP_STORAGE related headers
 * before asterisk.h since asterisk.h includes logger.h. logger.h
 * and c-client.h have conflicting definitions for LOG_WARNING and
 * LOG_DEBUG, so it's important that we use Asterisk's definitions
 * here instead of the c-client's 
 */
#ifdef IMAP_STORAGE
#include <ctype.h>
#include <signal.h>
#include <pwd.h>
#ifdef USE_SYSTEM_IMAP
#include <imap/c-client.h>
#include <imap/imap4r1.h>
#include <imap/linkage.h>
#elif defined (USE_SYSTEM_CCLIENT)
#include <c-client/c-client.h>
#include <c-client/imap4r1.h>
#include <c-client/linkage.h>
#else
#include "c-client.h"
#include "imap4r1.h"
#include "linkage.h"
#endif
#endif

#include "asterisk.h"

ASTERISK_FILE_VERSION(__FILE__, "$Revision: 136785 $")

#include "asterisk/paths.h"	/* use ast_config_AST_SPOOL_DIR */
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <dirent.h>

#include "asterisk/lock.h"
#include "asterisk/file.h"
#include "asterisk/channel.h"
#include "asterisk/pbx.h"
#include "asterisk/config.h"
#include "asterisk/say.h"
#include "asterisk/module.h"
#include "asterisk/adsi.h"
#include "asterisk/app.h"
#include "asterisk/manager.h"
#include "asterisk/dsp.h"
#include "asterisk/localtime.h"
#include "asterisk/cli.h"
#include "asterisk/utils.h"
#include "asterisk/stringfields.h"
#include "asterisk/smdi.h"
#include "asterisk/event.h"

#ifdef ODBC_STORAGE
#include "asterisk/res_odbc.h"
#endif

#ifdef IMAP_STORAGE
static char imapserver[48];
static char imapport[8];
static char imapflags[128];
static char imapfolder[64];
static char imapparentfolder[64] = "\0";
static char greetingfolder[64];
static char authuser[32];
static char authpassword[42];

static int expungeonhangup = 1;
static int imapgreetings = 0;
static char delimiter = '\0';

struct vm_state;
struct ast_vm_user;

/* Forward declarations for IMAP */
static int init_mailstream(struct vm_state *vms, int box);
static void write_file(char *filename, char *buffer, unsigned long len);
static char *get_header_by_tag(char *header, char *tag, char *buf, size_t len);
static void vm_imap_delete(int msgnum, struct ast_vm_user *vmu);
static char *get_user_by_mailbox(char *mailbox, char *buf, size_t len);
static struct vm_state *get_vm_state_by_imapuser(char *user, int interactive);
static struct vm_state *get_vm_state_by_mailbox(const char *mailbox, int interactive);
static struct vm_state *create_vm_state_from_user(struct ast_vm_user *vmu);
static void vmstate_insert(struct vm_state *vms);
static void vmstate_delete(struct vm_state *vms);
static void set_update(MAILSTREAM * stream);
static void init_vm_state(struct vm_state *vms);
static int save_body(BODY *body, struct vm_state *vms, char *section, char *format);
static void get_mailbox_delimiter(MAILSTREAM *stream);
static void mm_parsequota (MAILSTREAM *stream, unsigned char *msg, QUOTALIST *pquota);
static void imap_mailbox_name(char *spec, size_t len, struct vm_state *vms, int box, int target);
static int imap_store_file(char *dir, char *mailboxuser, char *mailboxcontext, int msgnum, struct ast_channel *chan, struct ast_vm_user *vmu, char *fmt, int duration, struct vm_state *vms);
static void update_messages_by_imapuser(const char *user, unsigned long number);

static int imap_remove_file (char *dir, int msgnum);
static int imap_retrieve_file (const char *dir, const int msgnum, const char *mailbox, const char *context);
static int imap_delete_old_greeting (char *dir, struct vm_state *vms);
static void check_quota(struct vm_state *vms, char *mailbox);
static int open_mailbox(struct vm_state *vms, struct ast_vm_user *vmu, int box);
struct vmstate {
	struct vm_state *vms;
	AST_LIST_ENTRY(vmstate) list;
};

static AST_LIST_HEAD_STATIC(vmstates, vmstate);

#endif

#define SMDI_MWI_WAIT_TIMEOUT 1000 /* 1 second */

#define COMMAND_TIMEOUT 5000
/* Don't modify these here; set your umask at runtime instead */
#define	VOICEMAIL_DIR_MODE	0777
#define	VOICEMAIL_FILE_MODE	0666
#define	CHUNKSIZE	65536

#define VOICEMAIL_CONFIG "voicemail.conf"
#define ASTERISK_USERNAME "asterisk"

/* Define fast-forward, pause, restart, and reverse keys
   while listening to a voicemail message - these are
   strings, not characters */
#define DEFAULT_LISTEN_CONTROL_FORWARD_KEY "#"
#define DEFAULT_LISTEN_CONTROL_REVERSE_KEY "*"
#define DEFAULT_LISTEN_CONTROL_PAUSE_KEY "0"
#define DEFAULT_LISTEN_CONTROL_RESTART_KEY "2"
#define DEFAULT_LISTEN_CONTROL_STOP_KEY "13456789"
#define VALID_DTMF "1234567890*#" /* Yes ABCD are valid dtmf but what phones have those? */

/* Default mail command to mail voicemail. Change it with the
    mailcmd= command in voicemail.conf */
#define SENDMAIL "/usr/sbin/sendmail -t"

#define INTRO "vm-intro"

#define MAXMSG 100
#define MAXMSGLIMIT 9999

#define BASELINELEN 72
#define BASEMAXINLINE 256
#define eol "\r\n"

#define MAX_DATETIME_FORMAT	512
#define MAX_NUM_CID_CONTEXTS 10

#define VM_REVIEW        (1 << 0)
#define VM_OPERATOR      (1 << 1)
#define VM_SAYCID        (1 << 2)
#define VM_SVMAIL        (1 << 3)
#define VM_ENVELOPE      (1 << 4)
#define VM_SAYDURATION   (1 << 5)
#define VM_SKIPAFTERCMD  (1 << 6)
#define VM_FORCENAME     (1 << 7)   /*!< Have new users record their name */
#define VM_FORCEGREET    (1 << 8)   /*!< Have new users record their greetings */
#define VM_PBXSKIP       (1 << 9)
#define VM_DIRECFORWARD  (1 << 10)  /*!< directory_forward */
#define VM_ATTACH        (1 << 11)
#define VM_DELETE        (1 << 12)
#define VM_ALLOCED       (1 << 13)
#define VM_SEARCH        (1 << 14)
#define VM_TEMPGREETWARN (1 << 15)  /*!< Remind user tempgreeting is set */
#define VM_MOVEHEARD     (1 << 16)  /*!< Move a "heard" message to Old after listening to it */
#define ERROR_LOCK_PATH  -100
#define ERROR_MAILBOX_FULL	-200


enum {
	NEW_FOLDER,
	OLD_FOLDER,
	WORK_FOLDER,
	FAMILY_FOLDER,
	FRIENDS_FOLDER,
	GREETINGS_FOLDER
} vm_box;

enum {
	OPT_SILENT =           (1 << 0),
	OPT_BUSY_GREETING =    (1 << 1),
	OPT_UNAVAIL_GREETING = (1 << 2),
	OPT_RECORDGAIN =       (1 << 3),
	OPT_PREPEND_MAILBOX =  (1 << 4),
	OPT_AUTOPLAY =         (1 << 6),
	OPT_DTMFEXIT =         (1 << 7),
} vm_option_flags;

enum {
	OPT_ARG_RECORDGAIN = 0,
	OPT_ARG_PLAYFOLDER = 1,
	OPT_ARG_DTMFEXIT   = 2,
	/* This *must* be the last value in this enum! */
	OPT_ARG_ARRAY_SIZE = 3,
} vm_option_args;

AST_APP_OPTIONS(vm_app_options, {
	AST_APP_OPTION('s', OPT_SILENT),
	AST_APP_OPTION('b', OPT_BUSY_GREETING),
	AST_APP_OPTION('u', OPT_UNAVAIL_GREETING),
	AST_APP_OPTION_ARG('g', OPT_RECORDGAIN, OPT_ARG_RECORDGAIN),
	AST_APP_OPTION_ARG('d', OPT_DTMFEXIT, OPT_ARG_DTMFEXIT),
	AST_APP_OPTION('p', OPT_PREPEND_MAILBOX),
	AST_APP_OPTION_ARG('a', OPT_AUTOPLAY, OPT_ARG_PLAYFOLDER),
});

static int load_config(int reload);

/*! \page vmlang Voicemail Language Syntaxes Supported

	\par Syntaxes supported, not really language codes.
	\arg \b en    - English
	\arg \b de    - German
	\arg \b es    - Spanish
	\arg \b fr    - French
	\arg \b it    - Italian
	\arg \b nl    - Dutch
	\arg \b pt    - Portuguese
	\arg \b pt_BR - Portuguese (Brazil)
	\arg \b gr    - Greek
	\arg \b no    - Norwegian
	\arg \b se    - Swedish
	\arg \b tw    - Chinese (Taiwan)
	\arg \b ua - Ukrainian

German requires the following additional soundfile:
\arg \b 1F	einE (feminine)

Spanish requires the following additional soundfile:
\arg \b 1M      un (masculine)

Dutch, Portuguese & Spanish require the following additional soundfiles:
\arg \b vm-INBOXs	singular of 'new'
\arg \b vm-Olds		singular of 'old/heard/read'

NB these are plural:
\arg \b vm-INBOX	nieuwe (nl)
\arg \b vm-Old		oude (nl)

Polish uses:
\arg \b vm-new-a	'new', feminine singular accusative
\arg \b vm-new-e	'new', feminine plural accusative
\arg \b vm-new-ych	'new', feminine plural genitive
\arg \b vm-old-a	'old', feminine singular accusative
\arg \b vm-old-e	'old', feminine plural accusative
\arg \b vm-old-ych	'old', feminine plural genitive
\arg \b digits/1-a	'one', not always same as 'digits/1'
\arg \b digits/2-ie	'two', not always same as 'digits/2'

Swedish uses:
\arg \b vm-nytt		singular of 'new'
\arg \b vm-nya		plural of 'new'
\arg \b vm-gammalt	singular of 'old'
\arg \b vm-gamla	plural of 'old'
\arg \b digits/ett	'one', not always same as 'digits/1'

Norwegian uses:
\arg \b vm-ny		singular of 'new'
\arg \b vm-nye		plural of 'new'
\arg \b vm-gammel	singular of 'old'
\arg \b vm-gamle	plural of 'old'

Dutch also uses:
\arg \b nl-om		'at'?

Spanish also uses:
\arg \b vm-youhaveno

Ukrainian requires the following additional soundfile:
\arg \b vm-nove		'nove'
\arg \b vm-stare	'stare'
\arg \b digits/ua/1e	'odne'

Italian requires the following additional soundfile:

For vm_intro_it:
\arg \b vm-nuovo	new
\arg \b vm-nuovi	new plural
\arg \b vm-vecchio	old
\arg \b vm-vecchi	old plural

Chinese (Taiwan) requires the following additional soundfile:
\arg \b vm-tong		A class-word for call (tong1)
\arg \b vm-ri		A class-word for day (ri4)
\arg \b vm-you		You (ni3)
\arg \b vm-haveno   Have no (mei2 you3)
\arg \b vm-have     Have (you3)
\arg \b vm-listen   To listen (yao4 ting1)


\note Don't use vm-INBOX or vm-Old, because they are the name of the INBOX and Old folders,
spelled among others when you have to change folder. For the above reasons, vm-INBOX
and vm-Old are spelled plural, to make them sound more as folder name than an adjective.

*/

struct baseio {
	int iocp;
	int iolen;
	int linelength;
	int ateof;
	unsigned char iobuf[BASEMAXINLINE];
};

/*! Structure for linked list of users 
 * Use ast_vm_user_destroy() to free one of these structures. */
struct ast_vm_user {
	char context[AST_MAX_CONTEXT];   /*!< Voicemail context */
	char mailbox[AST_MAX_EXTENSION]; /*!< Mailbox id, unique within vm context */
	char password[80];               /*!< Secret pin code, numbers only */
	char fullname[80];               /*!< Full name, for directory app */
	char email[80];                  /*!< E-mail address */
	char pager[80];                  /*!< E-mail address to pager (no attachment) */
	char serveremail[80];            /*!< From: Mail address */
	char mailcmd[160];               /*!< Configurable mail command */
	char language[MAX_LANGUAGE];     /*!< Config: Language setting */
	char zonetag[80];                /*!< Time zone */
	char callback[80];
	char dialout[80];
	char uniqueid[80];               /*!< Unique integer identifier */
	char exit[80];
	char attachfmt[20];              /*!< Attachment format */
	unsigned int flags;              /*!< VM_ flags */	
	int saydurationm;
	int maxmsg;                      /*!< Maximum number of msgs per folder for this mailbox */
	int maxdeletedmsg;               /*!< Maximum number of deleted msgs saved for this mailbox */
	int maxsecs;                     /*!< Maximum number of seconds per message for this mailbox */
#ifdef IMAP_STORAGE
	char imapuser[80];               /*!< IMAP server login */
	char imappassword[80];           /*!< IMAP server password if authpassword not defined */
#endif
	double volgain;                  /*!< Volume gain for voicemails sent via email */
	AST_LIST_ENTRY(ast_vm_user) list;
};

/*! Voicemail time zones */
struct vm_zone {
	AST_LIST_ENTRY(vm_zone) list;
	char name[80];
	char timezone[80];
	char msg_format[512];
};

/*! Voicemail mailbox state */
struct vm_state {
	char curbox[80];
	char username[80];
	char curdir[PATH_MAX];
	char vmbox[PATH_MAX];
	char fn[PATH_MAX];
	char fn2[PATH_MAX];
	int *deleted;
	int *heard;
	int curmsg;
	int lastmsg;
	int newmessages;
	int oldmessages;
	int starting;
	int repeats;
#ifdef IMAP_STORAGE
	ast_mutex_t lock;
	int updated;                         /*!< decremented on each mail check until 1 -allows delay */
	long msgArray[256];
	MAILSTREAM *mailstream;
	int vmArrayIndex;
	char imapuser[80];                   /*!< IMAP server login */
	int interactive;
	unsigned int quota_limit;
	unsigned int quota_usage;
	struct vm_state *persist_vms;
#endif
};

#ifdef ODBC_STORAGE
static char odbc_database[80];
static char odbc_table[80];
#define RETRIEVE(a,b,c,d) retrieve_file(a,b)
#define DISPOSE(a,b) remove_file(a,b)
#define STORE(a,b,c,d,e,f,g,h,i) store_file(a,b,c,d)
#define EXISTS(a,b,c,d) (message_exists(a,b))
#define RENAME(a,b,c,d,e,f,g,h) (rename_file(a,b,c,d,e,f))
#define COPY(a,b,c,d,e,f,g,h) (copy_file(a,b,c,d,e,f))
#define DELETE(a,b,c,d) (delete_file(a,b))
#else
#ifdef IMAP_STORAGE
#define DISPOSE(a,b) (imap_remove_file(a,b))
#define STORE(a,b,c,d,e,f,g,h,i) (imap_store_file(a,b,c,d,e,f,g,h,i))
#define EXISTS(a,b,c,d) (ast_fileexists(c, NULL, d) > 0)
#define RETRIEVE(a,b,c,d) imap_retrieve_file(a,b,c,d)
#define RENAME(a,b,c,d,e,f,g,h) (rename_file(g,h));
#define COPY(a,b,c,d,e,f,g,h) (copy_file(g,h));
#define DELETE(a,b,c,d) (vm_imap_delete(b,d))
#else
#define RETRIEVE(a,b,c,d)
#define DISPOSE(a,b)
#define STORE(a,b,c,d,e,f,g,h,i)
#define EXISTS(a,b,c,d) (ast_fileexists(c, NULL, d) > 0)
#define RENAME(a,b,c,d,e,f,g,h) (rename_file(g,h));
#define COPY(a,b,c,d,e,f,g,h) (copy_plain_file(g,h));
#define DELETE(a,b,c,d) (vm_delete(c))
#endif
#endif

static char VM_SPOOL_DIR[PATH_MAX];

static char ext_pass_cmd[128];

int my_umask;

#define PWDCHANGE_INTERNAL (1 << 1)
#define PWDCHANGE_EXTERNAL (1 << 2)
static int pwdchange = PWDCHANGE_INTERNAL;

#ifdef ODBC_STORAGE
#define tdesc "Comedian Mail (Voicemail System) with ODBC Storage"
#else
# ifdef IMAP_STORAGE
# define tdesc "Comedian Mail (Voicemail System) with IMAP Storage"
# else
# define tdesc "Comedian Mail (Voicemail System)"
# endif
#endif

static char userscontext[AST_MAX_EXTENSION] = "default";

static char *addesc = "Comedian Mail";

static char *synopsis_vm = "Leave a Voicemail message";

static char *descrip_vm =
	"  VoiceMail(mailbox[@context][&mailbox[@context]][...][,options]): This\n"
	"application allows the calling party to leave a message for the specified\n"
	"list of mailboxes. When multiple mailboxes are specified, the greeting will\n"
	"be taken from the first mailbox specified. Dialplan execution will stop if the\n"
	"specified mailbox does not exist.\n"
	"  The Voicemail application will exit if any of the following DTMF digits are\n"
	"received:\n"
	"    0 - Jump to the 'o' extension in the current dialplan context.\n"
	"    * - Jump to the 'a' extension in the current dialplan context.\n"
	"  This application will set the following channel variable upon completion:\n"
	"    VMSTATUS - This indicates the status of the execution of the VoiceMail\n"
	"               application. The possible values are:\n"
	"               SUCCESS | USEREXIT | FAILED\n\n"
	"  Options:\n"
	"    b      - Play the 'busy' greeting to the calling party.\n"
	"    d([c]) - Accept digits for a new extension in context c, if played during\n"
	"             the greeting.  Context defaults to the current context.\n"
	"    g(#)   - Use the specified amount of gain when recording the voicemail\n"
	"             message. The units are whole-number decibels (dB).\n"
	"             Only works on supported technologies, which is DAHDI only.\n"
	"    s      - Skip the playback of instructions for leaving a message to the\n"
	"             calling party.\n"
	"    u      - Play the 'unavailable' greeting.\n";

static char *synopsis_vmain = "Check Voicemail messages";

static char *descrip_vmain =
	"  VoiceMailMain([mailbox][@context][,options]): This application allows the\n"
	"calling party to check voicemail messages. A specific mailbox, and optional\n"
	"corresponding context, may be specified. If a mailbox is not provided, the\n"
	"calling party will be prompted to enter one. If a context is not specified,\n"
	"the 'default' context will be used.\n\n"
	"  Options:\n"
	"    p    - Consider the mailbox parameter as a prefix to the mailbox that\n"
	"           is entered by the caller.\n"
	"    g(#) - Use the specified amount of gain when recording a voicemail\n"
	"           message. The units are whole-number decibels (dB).\n"
	"    s    - Skip checking the passcode for the mailbox.\n"
	"    a(#) - Skip folder prompt and go directly to folder specified.\n"
	"           Defaults to INBOX\n";

static char *synopsis_vm_box_exists =
"Check to see if Voicemail mailbox exists";

static char *descrip_vm_box_exists =
	"  MailboxExists(mailbox[@context][,options]): Check to see if the specified\n"
	"mailbox exists. If no voicemail context is specified, the 'default' context\n"
	"will be used.\n"
	"  This application will set the following channel variable upon completion:\n"
	"    VMBOXEXISTSSTATUS - This will contain the status of the execution of the\n"
	"                        MailboxExists application. Possible values include:\n"
	"                        SUCCESS | FAILED\n\n"
	"  Options: (none)\n";

static char *synopsis_vmauthenticate = "Authenticate with Voicemail passwords";

static char *descrip_vmauthenticate =
	"  VMAuthenticate([mailbox][@context][,options]): This application behaves the\n"
	"same way as the Authenticate application, but the passwords are taken from\n"
	"voicemail.conf.\n"
	"  If the mailbox is specified, only that mailbox's password will be considered\n"
	"valid. If the mailbox is not specified, the channel variable AUTH_MAILBOX will\n"
	"be set with the authenticated mailbox.\n\n"
	"  Options:\n"
	"    s - Skip playing the initial prompts.\n";

/* Leave a message */
static char *app = "VoiceMail";

/* Check mail, control, etc */
static char *app2 = "VoiceMailMain";

static char *app3 = "MailboxExists";
static char *app4 = "VMAuthenticate";

static AST_LIST_HEAD_STATIC(users, ast_vm_user);
static AST_LIST_HEAD_STATIC(zones, vm_zone);
static int maxsilence;
static int maxmsg;
static int maxdeletedmsg;
static int silencethreshold = 128;
static char serveremail[80];
static char mailcmd[160];	/* Configurable mail cmd */
static char externnotify[160]; 
static struct ast_smdi_interface *smdi_iface = NULL;
static char vmfmts[80];
static double volgain;
static int vmminsecs;
static int vmmaxsecs;
static int maxgreet;
static int skipms;
static int maxlogins;

/*! Poll mailboxes for changes since there is something external to
 *  app_voicemail that may change them. */
static unsigned int poll_mailboxes;

/*! Polling frequency */
static unsigned int poll_freq;
/*! By default, poll every 30 seconds */
#define DEFAULT_POLL_FREQ 30

AST_MUTEX_DEFINE_STATIC(poll_lock);
static ast_cond_t poll_cond = PTHREAD_COND_INITIALIZER;
static pthread_t poll_thread = AST_PTHREADT_NULL;
static unsigned char poll_thread_run;

/*! Subscription to ... MWI event subscriptions */
static struct ast_event_sub *mwi_sub_sub;
/*! Subscription to ... MWI event un-subscriptions */
static struct ast_event_sub *mwi_unsub_sub;

/*!
 * \brief An MWI subscription
 *
 * This is so we can keep track of which mailboxes are subscribed to.
 * This way, we know which mailboxes to poll when the pollmailboxes
 * option is being used.
 */
struct mwi_sub {
	AST_RWLIST_ENTRY(mwi_sub) entry;
	int old_new;
	int old_old;
	uint32_t uniqueid;
	char mailbox[1];
};

static AST_RWLIST_HEAD_STATIC(mwi_subs, mwi_sub);

/* custom audio control prompts for voicemail playback */
static char listen_control_forward_key[12];
static char listen_control_reverse_key[12];
static char listen_control_pause_key[12];
static char listen_control_restart_key[12];
static char listen_control_stop_key[12];

/* custom password sounds */
static char vm_password[80] = "vm-password";
static char vm_newpassword[80] = "vm-newpassword";
static char vm_passchanged[80] = "vm-passchanged";
static char vm_reenterpassword[80] = "vm-reenterpassword";
static char vm_mismatch[80] = "vm-mismatch";

static struct ast_flags globalflags = {0};

static int saydurationminfo;

static char dialcontext[AST_MAX_CONTEXT] = "";
static char callcontext[AST_MAX_CONTEXT] = "";
static char exitcontext[AST_MAX_CONTEXT] = "";

static char cidinternalcontexts[MAX_NUM_CID_CONTEXTS][64];


static char *emailbody = NULL;
static char *emailsubject = NULL;
static char *pagerbody = NULL;
static char *pagersubject = NULL;
static char fromstring[100];
static char pagerfromstring[100];
static char charset[32] = "ISO-8859-1";

static unsigned char adsifdn[4] = "\x00\x00\x00\x0F";
static unsigned char adsisec[4] = "\x9B\xDB\xF7\xAC";
static int adsiver = 1;
static char emaildateformat[32] = "%A, %B %d, %Y at %r";

/* Forward declarations - generic */
static int open_mailbox(struct vm_state *vms, struct ast_vm_user *vmu, int box);
static int advanced_options(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, int msg, int option, signed char record_gain);
static int dialout(struct ast_channel *chan, struct ast_vm_user *vmu, char *num, char *outgoing_context);
static int play_record_review(struct ast_channel *chan, char *playfile, char *recordfile, int maxtime,
			char *fmt, int outsidecaller, struct ast_vm_user *vmu, int *duration, const char *unlockdir,
			signed char record_gain, struct vm_state *vms);
static int vm_tempgreeting(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain);
static int vm_play_folder_name(struct ast_channel *chan, char *mbox);
static int notify_new_message(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, int msgnum, long duration, char *fmt, char *cidnum, char *cidname);
static void make_email_file(FILE *p, char *srcemail, struct ast_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *attach, char *format, int duration, int attach_user_voicemail, struct ast_channel *chan, const char *category, int imap);
static void apply_options(struct ast_vm_user *vmu, const char *options);
static int is_valid_dtmf(const char *key);

#if !(defined(ODBC_STORAGE) || defined(IMAP_STORAGE))
static int __has_voicemail(const char *context, const char *mailbox, const char *folder, int shortcircuit);
#endif

static char *strip_control(const char *input, char *buf, size_t buflen)
{
	char *bufptr = buf;
	for (; *input; input++) {
		if (*input < 32) {
			continue;
		}
		*bufptr++ = *input;
		if (bufptr == buf + buflen - 1) {
			break;
		}
	}
	*bufptr = '\0';
	return buf;
}


static void populate_defaults(struct ast_vm_user *vmu)
{
	ast_copy_flags(vmu, (&globalflags), AST_FLAGS_ALL);	
	if (saydurationminfo)
		vmu->saydurationm = saydurationminfo;
	ast_copy_string(vmu->callback, callcontext, sizeof(vmu->callback));
	ast_copy_string(vmu->dialout, dialcontext, sizeof(vmu->dialout));
	ast_copy_string(vmu->exit, exitcontext, sizeof(vmu->exit));
	if (vmmaxsecs)
		vmu->maxsecs = vmmaxsecs;
	if (maxmsg)
		vmu->maxmsg = maxmsg;
	if (maxdeletedmsg)
		vmu->maxdeletedmsg = maxdeletedmsg;
	vmu->volgain = volgain;
}

static void apply_option(struct ast_vm_user *vmu, const char *var, const char *value)
{
	int x;
	if (!strcasecmp(var, "attach")) {
		ast_set2_flag(vmu, ast_true(value), VM_ATTACH);
	} else if (!strcasecmp(var, "attachfmt")) {
		ast_copy_string(vmu->attachfmt, value, sizeof(vmu->attachfmt));
	} else if (!strcasecmp(var, "serveremail")) {
		ast_copy_string(vmu->serveremail, value, sizeof(vmu->serveremail));
	} else if (!strcasecmp(var, "language")) {
		ast_copy_string(vmu->language, value, sizeof(vmu->language));
	} else if (!strcasecmp(var, "tz")) {
		ast_copy_string(vmu->zonetag, value, sizeof(vmu->zonetag));
#ifdef IMAP_STORAGE
	} else if (!strcasecmp(var, "imapuser")) {
		ast_copy_string(vmu->imapuser, value, sizeof(vmu->imapuser));
	} else if (!strcasecmp(var, "imappassword")) {
		ast_copy_string(vmu->imappassword, value, sizeof(vmu->imappassword));
#endif
	} else if (!strcasecmp(var, "delete") || !strcasecmp(var, "deletevoicemail")) {
		ast_set2_flag(vmu, ast_true(value), VM_DELETE);	
	} else if (!strcasecmp(var, "saycid")) {
		ast_set2_flag(vmu, ast_true(value), VM_SAYCID);	
	} else if (!strcasecmp(var, "sendvoicemail")) {
		ast_set2_flag(vmu, ast_true(value), VM_SVMAIL);	
	} else if (!strcasecmp(var, "review")) {
		ast_set2_flag(vmu, ast_true(value), VM_REVIEW);
	} else if (!strcasecmp(var, "tempgreetwarn")) {
		ast_set2_flag(vmu, ast_true(value), VM_TEMPGREETWARN);	
	} else if (!strcasecmp(var, "operator")) {
		ast_set2_flag(vmu, ast_true(value), VM_OPERATOR);	
	} else if (!strcasecmp(var, "envelope")) {
		ast_set2_flag(vmu, ast_true(value), VM_ENVELOPE);	
	} else if (!strcasecmp(var, "moveheard")) {
		ast_set2_flag(vmu, ast_true(value), VM_MOVEHEARD);
	} else if (!strcasecmp(var, "sayduration")) {
		ast_set2_flag(vmu, ast_true(value), VM_SAYDURATION);	
	} else if (!strcasecmp(var, "saydurationm")) {
		if (sscanf(value, "%d", &x) == 1) {
			vmu->saydurationm = x;
		} else {
			ast_log(LOG_WARNING, "Invalid min duration for say duration\n");
		}
	} else if (!strcasecmp(var, "forcename")) {
		ast_set2_flag(vmu, ast_true(value), VM_FORCENAME);	
	} else if (!strcasecmp(var, "forcegreetings")) {
		ast_set2_flag(vmu, ast_true(value), VM_FORCEGREET);	
	} else if (!strcasecmp(var, "callback")) {
		ast_copy_string(vmu->callback, value, sizeof(vmu->callback));
	} else if (!strcasecmp(var, "dialout")) {
		ast_copy_string(vmu->dialout, value, sizeof(vmu->dialout));
	} else if (!strcasecmp(var, "exitcontext")) {
		ast_copy_string(vmu->exit, value, sizeof(vmu->exit));
	} else if (!strcasecmp(var, "maxmessage") || !strcasecmp(var, "maxsecs")) {
		if (vmu->maxsecs <= 0) {
			ast_log(LOG_WARNING, "Invalid max message length of %s. Using global value %d\n", value, vmmaxsecs);
			vmu->maxsecs = vmmaxsecs;
		} else {
			vmu->maxsecs = atoi(value);
		}
		if (!strcasecmp(var, "maxmessage"))
			ast_log(LOG_WARNING, "Option 'maxmessage' has been deprecated in favor of 'maxsecs'.  Please make that change in your voicemail config.\n");
	} else if (!strcasecmp(var, "maxmsg")) {
		vmu->maxmsg = atoi(value);
		if (vmu->maxmsg <= 0) {
			ast_log(LOG_WARNING, "Invalid number of messages per folder maxmsg=%s. Using default value %d\n", value, MAXMSG);
			vmu->maxmsg = MAXMSG;
		} else if (vmu->maxmsg > MAXMSGLIMIT) {
			ast_log(LOG_WARNING, "Maximum number of messages per folder is %d. Cannot accept value maxmsg=%s\n", MAXMSGLIMIT, value);
			vmu->maxmsg = MAXMSGLIMIT;
		}
	} else if (!strcasecmp(var, "backupdeleted")) {
		if (sscanf(value, "%d", &x) == 1)
			vmu->maxdeletedmsg = x;
		else if (ast_true(value))
			vmu->maxdeletedmsg = MAXMSG;
		else
			vmu->maxdeletedmsg = 0;

		if (vmu->maxdeletedmsg < 0) {
			ast_log(LOG_WARNING, "Invalid number of deleted messages saved per mailbox backupdeleted=%s. Using default value %d\n", value, MAXMSG);
			vmu->maxdeletedmsg = MAXMSG;
		} else if (vmu->maxdeletedmsg > MAXMSGLIMIT) {
			ast_log(LOG_WARNING, "Maximum number of deleted messages saved per mailbox is %d. Cannot accept value backupdeleted=%s\n", MAXMSGLIMIT, value);
			vmu->maxdeletedmsg = MAXMSGLIMIT;
		}
	} else if (!strcasecmp(var, "volgain")) {
		sscanf(value, "%lf", &vmu->volgain);
	} else if (!strcasecmp(var, "options")) {
		apply_options(vmu, value);
	}
}

static int change_password_realtime(struct ast_vm_user *vmu, const char *password)
{
	int res;
	if (!ast_strlen_zero(vmu->uniqueid)) {
		res = ast_update_realtime("voicemail", "uniqueid", vmu->uniqueid, "password", password, NULL);
		if (res > 0) {
			ast_copy_string(vmu->password, password, sizeof(vmu->password));
			res = 0;
		} else if (!res) {
			res = -1;
		}
		return res;
	}
	return -1;
}

static void apply_options(struct ast_vm_user *vmu, const char *options)
{	/* Destructively Parse options and apply */
	char *stringp;
	char *s;
	char *var, *value;
	stringp = ast_strdupa(options);
	while ((s = strsep(&stringp, "|"))) {
		value = s;
		if ((var = strsep(&value, "=")) && value) {
			apply_option(vmu, var, value);
		}
	}	
}

static void apply_options_full(struct ast_vm_user *retval, struct ast_variable *var)
{
	struct ast_variable *tmp;
	tmp = var;
	while (tmp) {
		if (!strcasecmp(tmp->name, "vmsecret")) {
			ast_copy_string(retval->password, tmp->value, sizeof(retval->password));
		} else if (!strcasecmp(tmp->name, "secret") || !strcasecmp(tmp->name, "password")) { /* don't overwrite vmsecret if it exists */
			if (ast_strlen_zero(retval->password))
				ast_copy_string(retval->password, tmp->value, sizeof(retval->password));
		} else if (!strcasecmp(tmp->name, "uniqueid")) {
			ast_copy_string(retval->uniqueid, tmp->value, sizeof(retval->uniqueid));
		} else if (!strcasecmp(tmp->name, "pager")) {
			ast_copy_string(retval->pager, tmp->value, sizeof(retval->pager));
		} else if (!strcasecmp(tmp->name, "email")) {
			ast_copy_string(retval->email, tmp->value, sizeof(retval->email));
		} else if (!strcasecmp(tmp->name, "fullname")) {
			ast_copy_string(retval->fullname, tmp->value, sizeof(retval->fullname));
		} else if (!strcasecmp(tmp->name, "context")) {
			ast_copy_string(retval->context, tmp->value, sizeof(retval->context));
#ifdef IMAP_STORAGE
		} else if (!strcasecmp(tmp->name, "imapuser")) {
			ast_copy_string(retval->imapuser, tmp->value, sizeof(retval->imapuser));
		} else if (!strcasecmp(tmp->name, "imappassword")) {
			ast_copy_string(retval->imappassword, tmp->value, sizeof(retval->imappassword));
#endif
		} else
			apply_option(retval, tmp->name, tmp->value);
		tmp = tmp->next;
	} 
}

static int is_valid_dtmf(const char *key)
{
	int i;
	char *local_key = ast_strdupa(key);

	for (i = 0; i < strlen(key); ++i) {
		if (!strchr(VALID_DTMF, *local_key)) {
			ast_log(LOG_WARNING, "Invalid DTMF key \"%c\" used in voicemail configuration file\n", *local_key);
			return 0;
		}
		local_key++;
	}
	return 1;
}

static struct ast_vm_user *find_user_realtime(struct ast_vm_user *ivm, const char *context, const char *mailbox)
{
	struct ast_variable *var;
	struct ast_vm_user *retval;

	if ((retval = (ivm ? ivm : ast_calloc(1, sizeof(*retval))))) {
		if (!ivm)
			ast_set_flag(retval, VM_ALLOCED);	
		else
			memset(retval, 0, sizeof(*retval));
		if (mailbox) 
			ast_copy_string(retval->mailbox, mailbox, sizeof(retval->mailbox));
		populate_defaults(retval);
		if (!context && ast_test_flag((&globalflags), VM_SEARCH))
			var = ast_load_realtime("voicemail", "mailbox", mailbox, NULL);
		else
			var = ast_load_realtime("voicemail", "mailbox", mailbox, "context", context, NULL);
		if (var) {
			apply_options_full(retval, var);
			ast_variables_destroy(var);
		} else { 
			if (!ivm) 
				ast_free(retval);
			retval = NULL;
		}	
	} 
	return retval;
}

static struct ast_vm_user *find_user(struct ast_vm_user *ivm, const char *context, const char *mailbox)
{
	/* This function could be made to generate one from a database, too */
	struct ast_vm_user *vmu = NULL, *cur;
	AST_LIST_LOCK(&users);

	if (!context && !ast_test_flag((&globalflags), VM_SEARCH))
		context = "default";

	AST_LIST_TRAVERSE(&users, cur, list) {
		if (ast_test_flag((&globalflags), VM_SEARCH) && !strcasecmp(mailbox, cur->mailbox))
			break;
		if (context && (!strcasecmp(context, cur->context)) && (!strcasecmp(mailbox, cur->mailbox)))
			break;
	}
	if (cur) {
		/* Make a copy, so that on a reload, we have no race */
		if ((vmu = (ivm ? ivm : ast_malloc(sizeof(*vmu))))) {
			memcpy(vmu, cur, sizeof(*vmu));
			ast_set2_flag(vmu, !ivm, VM_ALLOCED);
			AST_LIST_NEXT(vmu, list) = NULL;
		}
	} else
		vmu = find_user_realtime(ivm, context, mailbox);
	AST_LIST_UNLOCK(&users);
	return vmu;
}

static int reset_user_pw(const char *context, const char *mailbox, const char *newpass)
{
	/* This function could be made to generate one from a database, too */
	struct ast_vm_user *cur;
	int res = -1;
	AST_LIST_LOCK(&users);
	AST_LIST_TRAVERSE(&users, cur, list) {
		if ((!context || !strcasecmp(context, cur->context)) &&
			(!strcasecmp(mailbox, cur->mailbox)))
				break;
	}
	if (cur) {
		ast_copy_string(cur->password, newpass, sizeof(cur->password));
		res = 0;
	}
	AST_LIST_UNLOCK(&users);
	return res;
}

static void vm_change_password(struct ast_vm_user *vmu, const char *newpassword)
{
	struct ast_config *cfg = NULL;
	struct ast_variable *var = NULL;
	struct ast_category *cat = NULL;
	char *category = NULL, *value = NULL, *new = NULL;
	const char *tmp = NULL;
	struct ast_flags config_flags = { CONFIG_FLAG_WITHCOMMENTS };
	if (!change_password_realtime(vmu, newpassword))
		return;

	/* check voicemail.conf */
	if ((cfg = ast_config_load(VOICEMAIL_CONFIG, config_flags))) {
		while ((category = ast_category_browse(cfg, category))) {
			if (!strcasecmp(category, vmu->context)) {
				if (!(tmp = ast_variable_retrieve(cfg, category, vmu->mailbox))) {
					ast_log(LOG_WARNING, "We could not find the mailbox.\n");
					break;
				}
				value = strstr(tmp, ",");
				if (!value) {
					ast_log(LOG_WARNING, "variable has bad format.\n");
					break;
				}
				new = alloca(strlen(value) + strlen(newpassword) + 1);
				sprintf(new, "%s%s", newpassword, value);
				if (!(cat = ast_category_get(cfg, category))) {
					ast_log(LOG_WARNING, "Failed to get category structure.\n");
					break;
				}
				ast_variable_update(cat, vmu->mailbox, new, NULL, 0);
			}
		}
		/* save the results */
		reset_user_pw(vmu->context, vmu->mailbox, newpassword);
		ast_copy_string(vmu->password, newpassword, sizeof(vmu->password));
		config_text_file_save(VOICEMAIL_CONFIG, cfg, "AppVoicemail");
	}
	category = NULL;
	var = NULL;
	/* check users.conf and update the password stored for the mailbox*/
	/* if no vmsecret entry exists create one. */
	if ((cfg = ast_config_load("users.conf", config_flags))) {
		ast_debug(4, "we are looking for %s\n", vmu->mailbox);
		while ((category = ast_category_browse(cfg, category))) {
			ast_debug(4, "users.conf: %s\n", category);
			if (!strcasecmp(category, vmu->mailbox)) {
				if (!(tmp = ast_variable_retrieve(cfg, category, "vmsecret"))) {
					ast_debug(3, "looks like we need to make vmsecret!\n");
					var = ast_variable_new("vmsecret", newpassword, "");
				} 
				new = alloca(strlen(newpassword) + 1);
				sprintf(new, "%s", newpassword);
				if (!(cat = ast_category_get(cfg, category))) {
					ast_debug(4, "failed to get category!\n");
					break;
				}
				if (!var)		
					ast_variable_update(cat, "vmsecret", new, NULL, 0);
				else
					ast_variable_append(cat, var);
			}
		}
		/* save the results and clean things up */
		reset_user_pw(vmu->context, vmu->mailbox, newpassword);	
		ast_copy_string(vmu->password, newpassword, sizeof(vmu->password));
		config_text_file_save("users.conf", cfg, "AppVoicemail");
	}
}

static void vm_change_password_shell(struct ast_vm_user *vmu, char *newpassword)
{
	char buf[255];
	snprintf(buf, sizeof(buf), "%s %s %s %s", ext_pass_cmd, vmu->context, vmu->mailbox, newpassword);
	if (!ast_safe_system(buf)) {
		ast_copy_string(vmu->password, newpassword, sizeof(vmu->password));
		/* Reset the password in memory, too */
		reset_user_pw(vmu->context, vmu->mailbox, newpassword);
	}
}

static int make_dir(char *dest, int len, const char *context, const char *ext, const char *folder)
{
	return snprintf(dest, len, "%s%s/%s/%s", VM_SPOOL_DIR, context, ext, folder);
}

/*! 
 * \brief Creates a file system path expression for a folder within the voicemail data folder and the appropriate context.
 * \param dest The variable to hold the output generated path expression. This buffer should be of size PATH_MAX.
 * \param len The length of the path string that was written out.
 * 
 * The path is constructed as 
 * 	VM_SPOOL_DIRcontext/ext/folder
 *
 * \return zero on success, -1 on error.
 */
static int make_file(char *dest, const int len, const char *dir, const int num)
{
	return snprintf(dest, len, "%s/msg%04d", dir, num);
}

/* same as mkstemp, but return a FILE * */
static FILE *vm_mkftemp(char *template)
{
	FILE *p = NULL;
	int pfd = mkstemp(template);
	chmod(template, VOICEMAIL_FILE_MODE & ~my_umask);
	if (pfd > -1) {
		p = fdopen(pfd, "w+");
		if (!p) {
			close(pfd);
			pfd = -1;
		}
	}
	return p;
}

/*! \brief basically mkdir -p $dest/$context/$ext/$folder
 * \param dest    String. base directory.
 * \param len     Length of dest.
 * \param context String. Ignored if is null or empty string.
 * \param ext     String. Ignored if is null or empty string.
 * \param folder  String. Ignored if is null or empty string. 
 * \return -1 on failure, 0 on success.
 */
static int create_dirpath(char *dest, int len, const char *context, const char *ext, const char *folder)
{
	mode_t	mode = VOICEMAIL_DIR_MODE;
	int res;

	make_dir(dest, len, context, ext, folder);
	if ((res = ast_mkdir(dest, mode))) {
		ast_log(LOG_WARNING, "ast_mkdir '%s' failed: %s\n", dest, strerror(res));
		return -1;
	}
	return 0;
}

static const char *mbox(int id)
{
	static const char *msgs[] = {
#ifdef IMAP_STORAGE
		imapfolder,
#else
		"INBOX",
#endif
		"Old",
		"Work",
		"Family",
		"Friends",
		"Cust1",
		"Cust2",
		"Cust3",
		"Cust4",
		"Cust5",
		"Deleted",
		"Urgent"
	};
	return (id >= 0 && id < (sizeof(msgs)/sizeof(msgs[0]))) ? msgs[id] : "Unknown";
}

static void free_user(struct ast_vm_user *vmu)
{
	if (ast_test_flag(vmu, VM_ALLOCED))
		ast_free(vmu);
}

/* All IMAP-specific functions should go in this block. This
* keeps them from being spread out all over the code */
#ifdef IMAP_STORAGE
static void vm_imap_delete(int msgnum, struct ast_vm_user *vmu)
{
	char arg[10];
	struct vm_state *vms;
	unsigned long messageNum;

	/* Greetings aren't stored in IMAP, so we can't delete them there */
	if (msgnum < 0) {
		return;
	}

	if (!(vms = get_vm_state_by_mailbox(vmu->mailbox, 1)) && !(vms = get_vm_state_by_mailbox(vmu->mailbox, 0))) {
		ast_log(LOG_WARNING, "Couldn't find a vm_state for mailbox %s. Unable to set \\DELETED flag for message %d\n", vmu->mailbox, msgnum);
		return;
	}

	/* find real message number based on msgnum */
	/* this may be an index into vms->msgArray based on the msgnum. */
	messageNum = vms->msgArray[msgnum];
	if (messageNum == 0) {
		ast_log(LOG_WARNING, "msgnum %d, mailbox message %lu is zero.\n", msgnum, messageNum);
		return;
	}
	ast_debug(3, "deleting msgnum %d, which is mailbox message %lu\n", msgnum, messageNum);
	/* delete message */
	snprintf(arg, sizeof(arg), "%lu", messageNum);
	mail_setflag(vms->mailstream, arg, "\\DELETED");
}

static int imap_retrieve_greeting (const char *dir, const int msgnum, struct ast_vm_user *vmu)
{
	struct vm_state *vms_p;
	char *file, *filename;
	char *attachment;
	int ret = 0, i;
	BODY *body;

	/* This function is only used for retrieval of IMAP greetings
	* regular messages are not retrieved this way, nor are greetings
	* if they are stored locally*/
	if (msgnum > -1 || !imapgreetings) {
		return 0;
	} else {
		file = strrchr(ast_strdupa(dir), '/');
		if (file)
			*file++ = '\0';
		else {
			ast_debug (1, "Failed to procure file name from directory passed.\n");
			return -1;
		}
	}

	/* check if someone is accessing this box right now... */
	if (!(vms_p = get_vm_state_by_mailbox(vmu->mailbox, 1)) ||!(vms_p = get_vm_state_by_mailbox(vmu->mailbox, 0))) {
		ast_log(LOG_ERROR, "Voicemail state not found!\n");
		return -1;
	}
	
	ret = init_mailstream(vms_p, GREETINGS_FOLDER);
	if (!vms_p->mailstream) {
		ast_log(LOG_ERROR, "IMAP mailstream is NULL\n");
		return -1;
	}

	/*XXX Yuck, this could probably be done a lot better */
	for (i = 0; i < vms_p->mailstream->nmsgs; i++) {
		mail_fetchstructure(vms_p->mailstream, i + 1, &body);
		/* We have the body, now we extract the file name of the first attachment. */
		if (body->nested.part && body->nested.part->next && body->nested.part->next->body.parameter->value) {
			attachment = ast_strdupa(body->nested.part->next->body.parameter->value);
		} else {
			ast_log(LOG_ERROR, "There is no file attached to this IMAP message.\n");
			return -1;
		}
		filename = strsep(&attachment, ".");
		if (!strcmp(filename, file)) {
			ast_copy_string(vms_p->fn, dir, sizeof(vms_p->fn));
			vms_p->msgArray[vms_p->curmsg] = i + 1;
			save_body(body, vms_p, "2", attachment);
			return 0;
		}
	}

	return -1;
}

static int imap_retrieve_file(const char *dir, const int msgnum, const char *mailbox, const char *context)
{
	BODY *body;
	char *header_content;
	char *attachedfilefmt;
	char buf[80];
	struct vm_state *vms;
	char text_file[PATH_MAX];
	FILE *text_file_ptr;
	int res = 0;
	struct ast_vm_user *vmu;

	if (!(vmu = find_user(NULL, context, mailbox))) {
		ast_log(LOG_WARNING, "Couldn't find user with mailbox %s@%s\n", mailbox, context);
		return -1;
	}
	
	if (msgnum < 0) {
		if (imapgreetings) {
			res = imap_retrieve_greeting(dir, msgnum, vmu);
			goto exit;
		} else {
			res = 0;
			goto exit;
		}
	}

	/* Before anything can happen, we need a vm_state so that we can
	* actually access the imap server through the vms->mailstream
	*/
	if (!(vms = get_vm_state_by_mailbox(vmu->mailbox, 1)) && !(vms = get_vm_state_by_mailbox(vmu->mailbox, 0))) {
		/* This should not happen. If it does, then I guess we'd
		* need to create the vm_state, extract which mailbox to
		* open, and then set up the msgArray so that the correct
		* IMAP message could be accessed. If I have seen correctly
		* though, the vms should be obtainable from the vmstates list
		* and should have its msgArray properly set up.
		*/
		ast_log(LOG_ERROR, "Couldn't find a vm_state for mailbox %s!!! Oh no!\n", vmu->mailbox);
		res = -1;
		goto exit;
	}
	
	make_file(vms->fn, sizeof(vms->fn), dir, msgnum);

	/* Don't try to retrieve a message from IMAP if it already is on the file system */
	if (ast_fileexists(vms->fn, NULL, NULL) > 0) {
		res = 0;
		goto exit;
	}

	if (option_debug > 2)
		ast_log (LOG_DEBUG,"Before mail_fetchheaders, curmsg is: %d, imap messages is %lu\n", msgnum, vms->msgArray[msgnum]);
	if (vms->msgArray[msgnum] == 0) {
		ast_log (LOG_WARNING,"Trying to access unknown message\n");
		res = -1;
		goto exit;
	}

	/* This will only work for new messages... */
	header_content = mail_fetchheader (vms->mailstream, vms->msgArray[msgnum]);
	/* empty string means no valid header */
	if (ast_strlen_zero(header_content)) {
		ast_log (LOG_ERROR,"Could not fetch header for message number %ld\n",vms->msgArray[msgnum]);
		res = -1;
		goto exit;
	}

	mail_fetchstructure (vms->mailstream,vms->msgArray[msgnum],&body);
	
	/* We have the body, now we extract the file name of the first attachment. */
	if (body->nested.part && body->nested.part->next && body->nested.part->next->body.parameter->value) {
		attachedfilefmt = ast_strdupa(body->nested.part->next->body.parameter->value);
	} else {
		ast_log(LOG_ERROR, "There is no file attached to this IMAP message.\n");
		res = -1;
		goto exit;
	}
	
	/* Find the format of the attached file */

	strsep(&attachedfilefmt, ".");
	if (!attachedfilefmt) {
		ast_log(LOG_ERROR, "File format could not be obtained from IMAP message attachment\n");
		res = -1;
		goto exit;
	}
	
	save_body(body, vms, "2", attachedfilefmt);

	/* Get info from headers!! */
	snprintf(text_file, sizeof(text_file), "%s.%s", vms->fn, "txt");

	if (!(text_file_ptr = fopen(text_file, "w"))) {
		ast_log(LOG_WARNING, "Unable to open/create file %s: %s\n", text_file, strerror(errno));
	}

	fprintf(text_file_ptr, "%s\n", "[message]");

	get_header_by_tag(header_content, "X-Asterisk-VM-Caller-ID-Num:", buf, sizeof(buf));
	fprintf(text_file_ptr, "callerid=\"%s\" ", S_OR(buf, ""));
	get_header_by_tag(header_content, "X-Asterisk-VM-Caller-ID-Name:", buf, sizeof(buf));
	fprintf(text_file_ptr, "<%s>\n", S_OR(buf, ""));
	get_header_by_tag(header_content, "X-Asterisk-VM-Context:", buf, sizeof(buf));
	fprintf(text_file_ptr, "context=%s\n", S_OR(buf, ""));
	get_header_by_tag(header_content, "X-Asterisk-VM-Orig-time:", buf, sizeof(buf));
	fprintf(text_file_ptr, "origtime=%s\n", S_OR(buf, ""));
	get_header_by_tag(header_content, "X-Asterisk-VM-Duration:", buf, sizeof(buf));
	fprintf(text_file_ptr, "duration=%s\n", S_OR(buf, ""));
	get_header_by_tag(header_content, "X-Asterisk-VM-Category:", buf, sizeof(buf));
	fprintf(text_file_ptr, "category=%s\n", S_OR(buf, ""));
	fclose(text_file_ptr);

exit:
	free_user(vmu);
	return res;
}

static int folder_int(const char *folder)
{
	/*assume a NULL folder means INBOX*/
	if (!folder)
		return 0;
	if (!strcasecmp(folder, imapfolder))
		return 0;
	else if (!strcasecmp(folder, "Old"))
		return 1;
	else if (!strcasecmp(folder, "Work"))
		return 2;
	else if (!strcasecmp(folder, "Family"))
		return 3;
	else if (!strcasecmp(folder, "Friends"))
		return 4;
	else if (!strcasecmp(folder, "Cust1"))
		return 5;
	else if (!strcasecmp(folder, "Cust2"))
		return 6;
	else if (!strcasecmp(folder, "Cust3"))
		return 7;
	else if (!strcasecmp(folder, "Cust4"))
		return 8;
	else if (!strcasecmp(folder, "Cust5"))
		return 9;
	else /*assume they meant INBOX if folder is not found otherwise*/
		return 0;
}

static int imap_store_file(char *dir, char *mailboxuser, char *mailboxcontext, int msgnum, struct ast_channel *chan, struct ast_vm_user *vmu, char *fmt, int duration, struct vm_state *vms)
{
	char *myserveremail = serveremail;
	char fn[PATH_MAX];
	char mailbox[256];
	char *stringp;
	FILE *p = NULL;
	char tmp[80] = "/tmp/astmail-XXXXXX";
	long len;
	void *buf;
	int tempcopy = 0;
	STRING str;
	
	/* Attach only the first format */
	fmt = ast_strdupa(fmt);
	stringp = fmt;
	strsep(&stringp, "|");

	if (!ast_strlen_zero(vmu->serveremail))
		myserveremail = vmu->serveremail;

	if (msgnum > -1)
		make_file(fn, sizeof(fn), dir, msgnum);
	else
		ast_copy_string (fn, dir, sizeof(fn));
	
	if (ast_strlen_zero(vmu->email)) {
		/* We need the vmu->email to be set when we call make_email_file, but
		* if we keep it set, a duplicate e-mail will be created. So at the end
		* of this function, we will revert back to an empty string if tempcopy
		* is 1.
		*/
		ast_copy_string(vmu->email, vmu->imapuser, sizeof(vmu->email));
		tempcopy = 1;
	}

	if (!strcmp(fmt, "wav49"))
		fmt = "WAV";
	ast_debug(3, "Storing file '%s', format '%s'\n", fn, fmt);

	/* Make a temporary file instead of piping directly to sendmail, in case the mail
	command hangs. */
	if (!(p = vm_mkftemp(tmp))) {
		ast_log(LOG_WARNING, "Unable to store '%s' (can't create temporary file)\n", fn);
		if (tempcopy)
			*(vmu->email) = '\0';
		return -1;
	}

	if (msgnum < 0 && imapgreetings) {
		init_mailstream(vms, GREETINGS_FOLDER);
		imap_delete_old_greeting(fn, vms);
	}
	
	make_email_file(p, myserveremail, vmu, msgnum, vmu->context, vmu->mailbox, S_OR(chan->cid.cid_num, NULL), S_OR(chan->cid.cid_name, NULL), fn, fmt, duration, 1, chan, NULL, 1);
	/* read mail file to memory */		
	len = ftell(p);
	rewind(p);
	if (!(buf = ast_malloc(len + 1))) {
		ast_log(LOG_ERROR, "Can't allocate %ld bytes to read message\n", len + 1);
		fclose(p);
		if (tempcopy)
			*(vmu->email) = '\0';
		return -1;
	}
	fread(buf, len, 1, p);
	((char *)buf)[len] = '\0';
	INIT(&str, mail_string, buf, len);
	init_mailstream(vms, NEW_FOLDER);
	imap_mailbox_name(mailbox, sizeof(mailbox), vms, NEW_FOLDER, 1);
	if (!mail_append(vms->mailstream, mailbox, &str))
		ast_log(LOG_ERROR, "Error while sending the message to %s\n", mailbox);
	fclose(p);
	unlink(tmp);
	ast_free(buf);
	ast_debug(3, "%s stored\n", fn);
	
	if (tempcopy)
		*(vmu->email) = '\0';
	
	return 0;

}

static int messagecount(const char *context, const char *mailbox, const char *folder)
{
	SEARCHPGM *pgm;
	SEARCHHEADER *hdr;

	struct ast_vm_user *vmu, vmus;
	struct vm_state *vms_p;
	int ret = 0;
	int fold = folder_int(folder);
	
	if (ast_strlen_zero(mailbox))
		return 0;

	/* We have to get the user before we can open the stream! */
	/* ast_log(LOG_DEBUG, "Before find_user, context is %s and mailbox is %s\n", context, mailbox); */
	vmu = find_user(&vmus, context, mailbox);
	if (!vmu) {
		ast_log(LOG_ERROR, "Couldn't find mailbox %s in context %s\n", mailbox, context);
		return -1;
	} else {
		/* No IMAP account available */
		if (vmu->imapuser[0] == '\0') {
			ast_log(LOG_WARNING, "IMAP user not set for mailbox %s\n", vmu->mailbox);
			return -1;
		}
	}
	
	/* No IMAP account available */
	if (vmu->imapuser[0] == '\0') {
		ast_log(LOG_WARNING, "IMAP user not set for mailbox %s\n", vmu->mailbox);
		free_user(vmu);
		return -1;
	}

	/* check if someone is accessing this box right now... */
	vms_p = get_vm_state_by_imapuser(vmu->imapuser, 1);
	if (!vms_p) {
		vms_p = get_vm_state_by_mailbox(mailbox, 1);
	}
	if (vms_p) {
		ast_debug(3, "Returning before search - user is logged in\n");
		if (fold == 0) { /* INBOX */
			return vms_p->newmessages;
		}
		if (fold == 1) { /* Old messages */
			return vms_p->oldmessages;
		}
	}

	/* add one if not there... */
	vms_p = get_vm_state_by_imapuser(vmu->imapuser, 0);
	if (!vms_p) {
		vms_p = get_vm_state_by_mailbox(mailbox, 0);
	}

	if (!vms_p) {
		ast_debug(3, "Adding new vmstate for %s\n", vmu->imapuser);
		if (!(vms_p = ast_calloc(1, sizeof(*vms_p)))) {
			return -1;
		}
		ast_copy_string(vms_p->imapuser, vmu->imapuser, sizeof(vms_p->imapuser));
		ast_copy_string(vms_p->username, mailbox, sizeof(vms_p->username)); /* save for access from interactive entry point */
		vms_p->mailstream = NIL; /* save for access from interactive entry point */
		ast_debug(3, "Copied %s to %s\n", vmu->imapuser, vms_p->imapuser);
		vms_p->updated = 1;
		/* set mailbox to INBOX! */
		ast_copy_string(vms_p->curbox, mbox(fold), sizeof(vms_p->curbox));
		init_vm_state(vms_p);
		vmstate_insert(vms_p);
	}
	ret = init_mailstream(vms_p, fold);
	if (!vms_p->mailstream) {
		ast_log(LOG_ERROR, "Houston we have a problem - IMAP mailstream is NULL\n");
		return -1;
	}
	if (ret == 0) {
		pgm = mail_newsearchpgm ();
		hdr = mail_newsearchheader ("X-Asterisk-VM-Extension", (char *)mailbox);
		pgm->header = hdr;
		if (fold != 1) {
			pgm->unseen = 1;
			pgm->seen = 0;
		}
		/* In the special case where fold is 1 (old messages) we have to do things a bit
		* differently. Old messages are stored in the INBOX but are marked as "seen"
		*/
		else {
			pgm->unseen = 0;
			pgm->seen = 1;
		}
		pgm->undeleted = 1;
		pgm->deleted = 0;

		vms_p->vmArrayIndex = 0;
		mail_search_full (vms_p->mailstream, NULL, pgm, NIL);
		if (fold == 0)
			vms_p->newmessages = vms_p->vmArrayIndex;
		if (fold == 1)
			vms_p->oldmessages = vms_p->vmArrayIndex;
		/* Freeing the searchpgm also frees the searchhdr */
		mail_free_searchpgm(&pgm);
		vms_p->updated = 0;
		return vms_p->vmArrayIndex;
	} else {  
		mail_ping(vms_p->mailstream);
	}
	return 0;
}
static int inboxcount(const char *mailbox_context, int *newmsgs, int *oldmsgs)
{
	char tmp[PATH_MAX] = "";
	char *mailboxnc;
	char *context;
	char *mb;
	char *cur;
	if (newmsgs)
		*newmsgs = 0;
	if (oldmsgs)
		*oldmsgs = 0;

	ast_debug(3, "Mailbox is set to %s\n", mailbox_context);
	/* If no mailbox, return immediately */
	if (ast_strlen_zero(mailbox_context))
		return 0;
	
	ast_copy_string(tmp, mailbox_context, sizeof(tmp));
	context = strchr(tmp, '@');
	if (strchr(mailbox_context, ',')) {
		int tmpnew, tmpold;
		ast_copy_string(tmp, mailbox_context, sizeof(tmp));
		mb = tmp;
		while ((cur = strsep(&mb, ", "))) {
			if (!ast_strlen_zero(cur)) {
				if (inboxcount(cur, newmsgs ? &tmpnew : NULL, oldmsgs ? &tmpold : NULL))
					return -1;
				else {
					if (newmsgs)
						*newmsgs += tmpnew; 
					if (oldmsgs)
						*oldmsgs += tmpold;
				}
			}
		}
		return 0;
	}
	if (context) {
		*context = '\0';
		mailboxnc = tmp;
		context++;
	} else {
		context = "default";
		mailboxnc = (char *)mailbox_context;
	}
	if (newmsgs) {
		if ((*newmsgs = messagecount(context, mailboxnc, imapfolder)) < 0)
			return -1;
	}
	if (oldmsgs) {
		if ((*oldmsgs = messagecount(context, mailboxnc, "Old")) < 0)
			return -1;
	}
	return 0;
}

/** 
* \brief Determines if the given folder has messages.
* \param mailbox The @ delimited string for user@context. If no context is found, uses 'default' for the context.
* \param folder the folder to look in
*
* This function is used when the mailbox is stored in an IMAP back end.
* This invokes the messagecount(). Here we are interested in the presence of messages (> 0) only, not the actual count.
* \return 1 if the folder has one or more messages. zero otherwise.
*/

static int has_voicemail(const char *mailbox, const char *folder)
{
	char tmp[256], *tmp2, *mbox, *context;
	ast_copy_string(tmp, mailbox, sizeof(tmp));
	tmp2 = tmp;
	if (strchr(tmp2, ',')) {
		while ((mbox = strsep(&tmp2, ","))) {
			if (!ast_strlen_zero(mbox)) {
				if (has_voicemail(mbox, folder))
					return 1;
			}
		}
	}
	if ((context= strchr(tmp, '@')))
		*context++ = '\0';
	else
		context = "default";
	return messagecount(context, tmp, folder) ? 1 : 0;
}

/*!
* \brief Copies a message from one mailbox to another.
* \param chan
* \param vmu
* \param imbox
* \param msgnum
* \param duration
* \param recip
* \param fmt
* \param dir
*
* This works with IMAP storage based mailboxes.
*
* \return zero on success, -1 on error.
*/
static int copy_message(struct ast_channel *chan, struct ast_vm_user *vmu, int imbox, int msgnum, long duration, struct ast_vm_user *recip, char *fmt, char *dir)
{
	struct vm_state *sendvms = NULL, *destvms = NULL;
	char messagestring[10]; /*I guess this could be a problem if someone has more than 999999999 messages...*/
	if (msgnum >= recip->maxmsg) {
		ast_log(LOG_WARNING, "Unable to copy mail, mailbox %s is full\n", recip->mailbox);
		return -1;
	}
	if (!(sendvms = get_vm_state_by_imapuser(vmu->imapuser, 0))) {
		ast_log(LOG_ERROR, "Couldn't get vm_state for originator's mailbox!!\n");
		return -1;
	}
	if (!(destvms = get_vm_state_by_imapuser(recip->imapuser, 0))) {
		ast_log(LOG_ERROR, "Couldn't get vm_state for destination mailbox!\n");
		return -1;
	}
	snprintf(messagestring, sizeof(messagestring), "%ld", sendvms->msgArray[msgnum]);
	if ((mail_copy(sendvms->mailstream, messagestring, (char *) mbox(imbox)) == T))
		return 0;
	ast_log(LOG_WARNING, "Unable to copy message from mailbox %s to mailbox %s\n", vmu->mailbox, recip->mailbox);
	return -1;
}

static void imap_mailbox_name(char *spec, size_t len, struct vm_state *vms, int box, int use_folder)
{
	char tmp[256], *t = tmp;
	size_t left = sizeof(tmp);
	
	if (box == OLD_FOLDER) {
		ast_copy_string(vms->curbox, mbox(NEW_FOLDER), sizeof(vms->curbox));
	} else {
		ast_copy_string(vms->curbox, mbox(box), sizeof(vms->curbox));
	}

	if (box == NEW_FOLDER) {
		ast_copy_string(vms->vmbox, "vm-INBOX", sizeof(vms->vmbox));
	} else {
		snprintf(vms->vmbox, sizeof(vms->vmbox), "vm-%s", mbox(box));
	}

	/* Build up server information */
	ast_build_string(&t, &left, "{%s:%s/imap", imapserver, imapport);

	/* Add authentication user if present */
	if (!ast_strlen_zero(authuser))
		ast_build_string(&t, &left, "/authuser=%s", authuser);

	/* Add flags if present */
	if (!ast_strlen_zero(imapflags))
		ast_build_string(&t, &left, "/%s", imapflags);

	/* End with username */
	ast_build_string(&t, &left, "/user=%s}", vms->imapuser);
	if (box == NEW_FOLDER || box == OLD_FOLDER)
		snprintf(spec, len, "%s%s", tmp, use_folder? imapfolder: "INBOX");
	else if (box == GREETINGS_FOLDER)
		snprintf(spec, len, "%s%s", tmp, greetingfolder);
	else {	/* Other folders such as Friends, Family, etc... */
		if (!ast_strlen_zero(imapparentfolder)) {
			/* imapparentfolder would typically be set to INBOX */
			snprintf(spec, len, "%s%s%c%s", tmp, imapparentfolder, delimiter, mbox(box));
		} else {
			snprintf(spec, len, "%s%s", tmp, mbox(box));
		}
	}
}

static int init_mailstream(struct vm_state *vms, int box)
{
	MAILSTREAM *stream = NIL;
	long debug;
	char tmp[256];
	
	if (!vms) {
		ast_log (LOG_ERROR,"vm_state is NULL!\n");
		return -1;
	}
	if (option_debug > 2)
		ast_log (LOG_DEBUG,"vm_state user is:%s\n",vms->imapuser);
	if (vms->mailstream == NIL || !vms->mailstream) {
		if (option_debug)
			ast_log (LOG_DEBUG,"mailstream not set.\n");
	} else {
		stream = vms->mailstream;
	}
	/* debug = T;  user wants protocol telemetry? */
	debug = NIL;  /* NO protocol telemetry? */

	if (delimiter == '\0') {		/* did not probe the server yet */
		char *cp;
#ifdef USE_SYSTEM_IMAP
#include <imap/linkage.c>
#elif defined(USE_SYSTEM_CCLIENT)
#include <c-client/linkage.c>
#else
#include "linkage.c"
#endif
		/* Connect to INBOX first to get folders delimiter */
		imap_mailbox_name(tmp, sizeof(tmp), vms, 0, 1);
		ast_mutex_lock(&vms->lock);
		stream = mail_open (stream, tmp, debug ? OP_DEBUG : NIL);
		ast_mutex_unlock(&vms->lock);
		if (stream == NIL) {
			ast_log (LOG_ERROR, "Can't connect to imap server %s\n", tmp);
			return -1;
		}
		get_mailbox_delimiter(stream);
		/* update delimiter in imapfolder */
		for (cp = imapfolder; *cp; cp++)
			if (*cp == '/')
				*cp = delimiter;
	}
	/* Now connect to the target folder */
	imap_mailbox_name(tmp, sizeof(tmp), vms, box, 1);
	if (option_debug > 2)
		ast_log (LOG_DEBUG,"Before mail_open, server: %s, box:%d\n", tmp, box);
	ast_mutex_lock(&vms->lock);
	vms->mailstream = mail_open (stream, tmp, debug ? OP_DEBUG : NIL);
	ast_mutex_unlock(&vms->lock);
	if (vms->mailstream == NIL) {
		return -1;
	} else {
		return 0;
	}
}

static int open_mailbox(struct vm_state *vms, struct ast_vm_user *vmu, int box)
{
	SEARCHPGM *pgm;
	SEARCHHEADER *hdr;
	int ret, urgent = 0;

	/* If Urgent, then look at INBOX */
	if (box == 11) {
		box = NEW_FOLDER;
		urgent = 1;
	}

	ast_copy_string(vms->imapuser,vmu->imapuser, sizeof(vms->imapuser));
	ast_debug(3,"Before init_mailstream, user is %s\n",vmu->imapuser);

	if ((ret = init_mailstream(vms, box)) || !vms->mailstream) {
		ast_log(LOG_ERROR, "Could not initialize mailstream\n");
		return -1;
	}
	
	create_dirpath(vms->curdir, sizeof(vms->curdir), vmu->context, vms->username, vms->curbox);
	
	/* Check Quota */
	if  (box == 0)  {
		ast_debug(3, "Mailbox name set to: %s, about to check quotas\n", mbox(box));
		check_quota(vms,(char *)mbox(box));
	}

	pgm = mail_newsearchpgm();

	/* Check IMAP folder for Asterisk messages only... */
	hdr = mail_newsearchheader("X-Asterisk-VM-Extension", vmu->mailbox);
	pgm->header = hdr;
	pgm->deleted = 0;
	pgm->undeleted = 1;

	/* if box = NEW_FOLDER, check for new, if box = OLD_FOLDER, check for read */
	if (box == NEW_FOLDER && urgent == 1) {
		pgm->unseen = 1;
		pgm->seen = 0;
		pgm->flagged = 1;
		pgm->unflagged = 0;
	} else if (box == NEW_FOLDER && urgent == 0) {
		pgm->unseen = 1;
		pgm->seen = 0;
		pgm->flagged = 0;
		pgm->unflagged = 1;
	} else if (box == OLD_FOLDER) {
		pgm->seen = 1;
		pgm->unseen = 0;
	}

	ast_debug(3,"Before mail_search_full, user is %s\n",vmu->imapuser);

	vms->vmArrayIndex = 0;
	mail_search_full (vms->mailstream, NULL, pgm, NIL);
	vms->lastmsg = vms->vmArrayIndex - 1;
	mail_free_searchpgm(&pgm);

	return 0;
}

static void write_file(char *filename, char *buffer, unsigned long len)
{
	FILE *output;

	output = fopen (filename, "w");
	fwrite (buffer, len, 1, output);
	fclose (output);
}

static void update_messages_by_imapuser(const char *user, unsigned long number)
{
	struct vmstate *vlist = NULL;

	AST_LIST_LOCK(&vmstates);
	AST_LIST_TRAVERSE(&vmstates, vlist, list) {
		if (!vlist->vms) {
			ast_debug(3, "error: vms is NULL for %s\n", user);
			continue;
		}
		if (!vlist->vms->imapuser) {
			ast_debug(3, "error: imapuser is NULL for %s\n", user);
			continue;
		}
		ast_debug(3, "saving mailbox message number %lu as message %d. Interactive set to %d\n", number, vlist->vms->vmArrayIndex, vlist->vms->interactive);
		vlist->vms->msgArray[vlist->vms->vmArrayIndex++] = number;
	}
	AST_LIST_UNLOCK(&vmstates);
}

void mm_searched(MAILSTREAM *stream, unsigned long number)
{
	char *mailbox = stream->mailbox, buf[1024] = "", *user;

	if (!(user = get_user_by_mailbox(mailbox, buf, sizeof(buf))))
		return;

	update_messages_by_imapuser(user, number);
}

static struct ast_vm_user *find_user_realtime_imapuser(const char *imapuser)
{
	struct ast_variable *var;
	struct ast_vm_user *vmu;

	vmu = ast_calloc(1, sizeof *vmu);
	if (!vmu)
		return NULL;
	ast_set_flag(vmu, VM_ALLOCED);
	populate_defaults(vmu);

	var = ast_load_realtime("voicemail", "imapuser", imapuser, NULL);
	if (var) {
		apply_options_full(vmu, var);
		ast_variables_destroy(var);
		return vmu;
	} else {
		free(vmu);
		return NULL;
	}
}

/* Interfaces to C-client */

void mm_exists(MAILSTREAM * stream, unsigned long number)
{
	/* mail_ping will callback here if new mail! */
	ast_debug(4, "Entering EXISTS callback for message %ld\n", number);
	if (number == 0) return;
	set_update(stream);
}


void mm_expunged(MAILSTREAM * stream, unsigned long number)
{
	/* mail_ping will callback here if expunged mail! */
	ast_debug(4, "Entering EXPUNGE callback for message %ld\n", number);
	if (number == 0) return;
	set_update(stream);
}


void mm_flags(MAILSTREAM * stream, unsigned long number)
{
	/* mail_ping will callback here if read mail! */
	ast_debug(4, "Entering FLAGS callback for message %ld\n", number);
	if (number == 0) return;
	set_update(stream);
}


void mm_notify(MAILSTREAM * stream, char *string, long errflg)
{
	ast_debug(5, "Entering NOTIFY callback, errflag is %ld, string is %s\n", errflg, string);
	mm_log (string, errflg);
}


void mm_list(MAILSTREAM * stream, int delim, char *mailbox, long attributes)
{
	if (delimiter == '\0') {
		delimiter = delim;
	}

	ast_debug(5, "Delimiter set to %c and mailbox %s\n",delim, mailbox);
	if (attributes & LATT_NOINFERIORS)
		ast_debug(5, "no inferiors\n");
	if (attributes & LATT_NOSELECT)
		ast_debug(5, "no select\n");
	if (attributes & LATT_MARKED)
		ast_debug(5, "marked\n");
	if (attributes & LATT_UNMARKED)
		ast_debug(5, "unmarked\n");
}


void mm_lsub(MAILSTREAM * stream, int delimiter, char *mailbox, long attributes)
{
	ast_debug(5, "Delimiter set to %c and mailbox %s\n",delimiter, mailbox);
	if (attributes & LATT_NOINFERIORS)
		ast_debug(5, "no inferiors\n");
	if (attributes & LATT_NOSELECT)
		ast_debug(5, "no select\n");
	if (attributes & LATT_MARKED)
		ast_debug(5, "marked\n");
	if (attributes & LATT_UNMARKED)
		ast_debug(5, "unmarked\n");
}


void mm_status(MAILSTREAM * stream, char *mailbox, MAILSTATUS * status)
{
	ast_log(LOG_NOTICE, " Mailbox %s", mailbox);
	if (status->flags & SA_MESSAGES)
		ast_log(LOG_NOTICE, ", %lu messages", status->messages);
	if (status->flags & SA_RECENT)
		ast_log(LOG_NOTICE, ", %lu recent", status->recent);
	if (status->flags & SA_UNSEEN)
		ast_log(LOG_NOTICE, ", %lu unseen", status->unseen);
	if (status->flags & SA_UIDVALIDITY)
		ast_log(LOG_NOTICE, ", %lu UID validity", status->uidvalidity);
	if (status->flags & SA_UIDNEXT)
		ast_log(LOG_NOTICE, ", %lu next UID", status->uidnext);
	ast_log(LOG_NOTICE, "\n");
}


void mm_log(char *string, long errflg)
{
	switch ((short) errflg) {
		case NIL:
			ast_debug(1,"IMAP Info: %s\n", string);
			break;
		case PARSE:
		case WARN:
			ast_log(LOG_WARNING, "IMAP Warning: %s\n", string);
			break;
		case ERROR:
			ast_log(LOG_ERROR, "IMAP Error: %s\n", string);
			break;
	}
}


void mm_dlog(char *string)
{
	ast_log(LOG_NOTICE, "%s\n", string);
}


void mm_login(NETMBX * mb, char *user, char *pwd, long trial)
{
	struct ast_vm_user *vmu;

	ast_debug(4, "Entering callback mm_login\n");

	ast_copy_string(user, mb->user, MAILTMPLEN);

	/* We should only do this when necessary */
	if (!ast_strlen_zero(authpassword)) {
		ast_copy_string(pwd, authpassword, MAILTMPLEN);
	} else {
		AST_LIST_TRAVERSE(&users, vmu, list) {
			if (!strcasecmp(mb->user, vmu->imapuser)) {
				ast_copy_string(pwd, vmu->imappassword, MAILTMPLEN);
				break;
			}
		}
		if (!vmu) {
			if ((vmu = find_user_realtime_imapuser(mb->user))) {
				ast_copy_string(pwd, vmu->imappassword, MAILTMPLEN);
				free_user(vmu);
			}
		}
	}
}


void mm_critical(MAILSTREAM * stream)
{
}


void mm_nocritical(MAILSTREAM * stream)
{
}


long mm_diskerror(MAILSTREAM * stream, long errcode, long serious)
{
	kill (getpid (), SIGSTOP);
	return NIL;
}


void mm_fatal(char *string)
{
	ast_log(LOG_ERROR, "IMAP access FATAL error: %s\n", string);
}

/* C-client callback to handle quota */
static void mm_parsequota(MAILSTREAM *stream, unsigned char *msg, QUOTALIST *pquota)
{
	struct vm_state *vms;
	char *mailbox = stream->mailbox, *user;
	char buf[1024] = "";
	unsigned long usage = 0, limit = 0;
	
	while (pquota) {
		usage = pquota->usage;
		limit = pquota->limit;
		pquota = pquota->next;
	}
	
	if (!(user = get_user_by_mailbox(mailbox, buf, sizeof(buf))) || !(vms = get_vm_state_by_imapuser(user, 2))) {
		ast_log(LOG_ERROR, "No state found.\n");
		return;
	}

	ast_debug(3, "User %s usage is %lu, limit is %lu\n", user, usage, limit);

	vms->quota_usage = usage;
	vms->quota_limit = limit;
}

static char *get_header_by_tag(char *header, char *tag, char *buf, size_t len)
{
	char *start, *eol_pnt;
	int taglen;

	if (ast_strlen_zero(header) || ast_strlen_zero(tag))
		return NULL;

	taglen = strlen(tag) + 1;
	if (taglen < 1)
		return NULL;

	if (!(start = strstr(header, tag)))
		return NULL;

	/* Since we can be called multiple times we should clear our buffer */
	memset(buf, 0, len);

	ast_copy_string(buf, start+taglen, len);
	if ((eol_pnt = strchr(buf,'\r')) || (eol_pnt = strchr(buf,'\n')))
		*eol_pnt = '\0';
	return buf;
}

static char *get_user_by_mailbox(char *mailbox, char *buf, size_t len)
{
	char *start, *quote, *eol_pnt;

	if (ast_strlen_zero(mailbox))
		return NULL;

	if (!(start = strstr(mailbox, "/user=")))
		return NULL;

	ast_copy_string(buf, start+6, len);

	if (!(quote = strchr(buf, '\"'))) {
		if (!(eol_pnt = strchr(buf, '/')))
			eol_pnt = strchr(buf,'}');
		*eol_pnt = '\0';
		return buf;
	} else {
		eol_pnt = strchr(buf+1,'\"');
		*eol_pnt = '\0';
		return buf+1;
	}
}

static struct vm_state *create_vm_state_from_user(struct ast_vm_user *vmu)
{
	struct vm_state *vms_p;

	if (option_debug > 4)
		ast_log(LOG_DEBUG,"Adding new vmstate for %s\n",vmu->imapuser);
	if (!(vms_p = ast_calloc(1, sizeof(*vms_p))))
		return NULL;
	ast_copy_string(vms_p->imapuser, vmu->imapuser, sizeof(vms_p->imapuser));
	ast_copy_string(vms_p->username, vmu->mailbox, sizeof(vms_p->username)); /* save for access from interactive entry point */
	vms_p->mailstream = NIL; /* save for access from interactive entry point */
	if (option_debug > 4)
		ast_log(LOG_DEBUG,"Copied %s to %s\n",vmu->imapuser,vms_p->imapuser);
	vms_p->updated = 1;
	/* set mailbox to INBOX! */
	ast_copy_string(vms_p->curbox, mbox(0), sizeof(vms_p->curbox));
	init_vm_state(vms_p);
	vmstate_insert(vms_p);
	return vms_p;
}

static struct vm_state *get_vm_state_by_imapuser(char *user, int interactive)
{
	struct vmstate *vlist = NULL;

	AST_LIST_LOCK(&vmstates);
	AST_LIST_TRAVERSE(&vmstates, vlist, list) {
		if (!vlist->vms) {
			ast_debug(3, "error: vms is NULL for %s\n", user);
			continue;
		}
		if (!vlist->vms->imapuser) {
			ast_debug(3, "error: imapuser is NULL for %s\n", user);
			continue;
		}

		if (!strcmp(vlist->vms->imapuser, user) && (interactive == 2 || vlist->vms->interactive == interactive)) {
			AST_LIST_UNLOCK(&vmstates);
			return vlist->vms;
		}
	}
	AST_LIST_UNLOCK(&vmstates);

	ast_debug(3, "%s not found in vmstates\n", user);

	return NULL;
}

static struct vm_state *get_vm_state_by_mailbox(const char *mailbox, int interactive)
{

	struct vmstate *vlist = NULL;

	AST_LIST_LOCK(&vmstates);
	AST_LIST_TRAVERSE(&vmstates, vlist, list) {
		if (!vlist->vms) {
			ast_debug(3, "error: vms is NULL for %s\n", mailbox);
			continue;
		}
		if (!vlist->vms->username) {
			ast_debug(3, "error: username is NULL for %s\n", mailbox);
			continue;
		}

		ast_debug(3, "comparing mailbox %s (i=%d) to vmstate mailbox %s (i=%d)\n", mailbox, interactive, vlist->vms->username, vlist->vms->interactive);
		
		if (!strcmp(vlist->vms->username,mailbox) && vlist->vms->interactive == interactive) {
			ast_debug(3, "Found it!\n");
			AST_LIST_UNLOCK(&vmstates);
			return vlist->vms;
		}
	}
	AST_LIST_UNLOCK(&vmstates);

	ast_debug(3, "%s not found in vmstates\n", mailbox);

	return NULL;
}

static void vmstate_insert(struct vm_state *vms) 
{
	struct vmstate *v;
	struct vm_state *altvms;

	/* If interactive, it probably already exists, and we should
	use the one we already have since it is more up to date.
	We can compare the username to find the duplicate */
	if (vms->interactive == 1) {
		altvms = get_vm_state_by_mailbox(vms->username,0);
		if (altvms) {	
			ast_debug(3, "Duplicate mailbox %s, copying message info...\n",vms->username);
			vms->newmessages = altvms->newmessages;
			vms->oldmessages = altvms->oldmessages;
			vms->vmArrayIndex = altvms->vmArrayIndex;
			vms->lastmsg = altvms->lastmsg;
			vms->curmsg = altvms->curmsg;
			/* get a pointer to the persistent store */
			vms->persist_vms = altvms;
			/* Reuse the mailstream? */
			vms->mailstream = altvms->mailstream;
			/* vms->mailstream = NIL; */
		}
	}

	if (!(v = ast_calloc(1, sizeof(*v))))
		return;
	
	v->vms = vms;

	ast_debug(3, "Inserting vm_state for user:%s, mailbox %s\n",vms->imapuser,vms->username);

	AST_LIST_LOCK(&vmstates);
	AST_LIST_INSERT_TAIL(&vmstates, v, list);
	AST_LIST_UNLOCK(&vmstates);
}

static void vmstate_delete(struct vm_state *vms) 
{
	struct vmstate *vc = NULL;
	struct vm_state *altvms = NULL;

	/* If interactive, we should copy pertinent info
	back to the persistent state (to make update immediate) */
	if (vms->interactive == 1 && (altvms = vms->persist_vms)) {
		ast_debug(3, "Duplicate mailbox %s, copying message info...\n", vms->username);
		altvms->newmessages = vms->newmessages;
		altvms->oldmessages = vms->oldmessages;
		altvms->updated = 1;
	}
	
	ast_debug(3, "Removing vm_state for user:%s, mailbox %s\n", vms->imapuser, vms->username);
	
	AST_LIST_LOCK(&vmstates);
	AST_LIST_TRAVERSE_SAFE_BEGIN(&vmstates, vc, list) {
		if (vc->vms == vms) {
			AST_LIST_REMOVE_CURRENT(list);
			break;
		}
	}
	AST_LIST_TRAVERSE_SAFE_END
	AST_LIST_UNLOCK(&vmstates);
	
	if (vc) {
		ast_mutex_destroy(&vc->vms->lock);
		ast_free(vc);
	}
	else
		ast_log(LOG_ERROR, "No vmstate found for user:%s, mailbox %s\n", vms->imapuser, vms->username);
}

static void set_update(MAILSTREAM * stream) 
{
	struct vm_state *vms;
	char *mailbox = stream->mailbox, *user;
	char buf[1024] = "";

	if (!(user = get_user_by_mailbox(mailbox, buf, sizeof(buf))) || !(vms = get_vm_state_by_imapuser(user, 0))) {
		if (user && option_debug > 2)
			ast_log(LOG_WARNING, "User %s mailbox not found for update.\n", user);
		return;
	}

	ast_debug(3, "User %s mailbox set for update.\n", user);

	vms->updated = 1; /* Set updated flag since mailbox changed */
}

static void init_vm_state(struct vm_state *vms) 
{
	int x;
	vms->vmArrayIndex = 0;
	for (x = 0; x < 256; x++) {
		vms->msgArray[x] = 0;
	}
	ast_mutex_init(&vms->lock);
}

static int save_body(BODY *body, struct vm_state *vms, char *section, char *format) 
{
	char *body_content;
	char *body_decoded;
	unsigned long len;
	unsigned long newlen;
	char filename[256];
	
	if (!body || body == NIL)
		return -1;
	body_content = mail_fetchbody (vms->mailstream, vms->msgArray[vms->curmsg], section, &len);
	if (body_content != NIL) {
		snprintf(filename, sizeof(filename), "%s.%s", vms->fn, format);
		/* ast_log (LOG_DEBUG,body_content); */
		body_decoded = rfc822_base64 ((unsigned char *)body_content, len, &newlen);
		write_file (filename, (char *) body_decoded, newlen);
	}
	return 0;
}
/*! 
* \brief Get delimiter via mm_list callback 
* \param stream
*
* Determines the delimiter character that is used by the underlying IMAP based mail store.
*/
static void get_mailbox_delimiter(MAILSTREAM *stream) {
	char tmp[50];
	snprintf(tmp, sizeof(tmp), "{%s}", imapserver);
	mail_list(stream, tmp, "*");
}

/*! 
* \brief Check Quota for user 
* \param vms a pointer to a vm_state struct, will use the mailstream property of this.
* \param mailbox the mailbox to check the quota for.
*
* Calls imap_getquotaroot, which will populate its results into the vm_state vms input structure.
*/
static void check_quota(struct vm_state *vms, char *mailbox) {
	mail_parameters(NULL, SET_QUOTA, (void *) mm_parsequota);
	ast_debug(3, "Mailbox name set to: %s, about to check quotas\n", mailbox);
	if (vms && vms->mailstream != NULL) {
		imap_getquotaroot(vms->mailstream, mailbox);
	} else {
		ast_log(LOG_WARNING, "Mailstream not available for mailbox: %s\n", mailbox);
	}
}

#endif /* IMAP_STORAGE */

	/*! \brief Lock file path
		only return failure if ast_lock_path returns 'timeout',
	not if the path does not exist or any other reason
	*/
	static int vm_lock_path(const char *path)
	{
		switch (ast_lock_path(path)) {
		case AST_LOCK_TIMEOUT:
			return -1;
		default:
			return 0;
		}
	}


#ifdef ODBC_STORAGE
struct generic_prepare_struct {
	char *sql;
	int argc;
	char **argv;
};

static SQLHSTMT generic_prepare(struct odbc_obj *obj, void *data)
{
	struct generic_prepare_struct *gps = data;
	int res, i;
	SQLHSTMT stmt;

	res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
		return NULL;
	}
	res = SQLPrepare(stmt, (unsigned char *)gps->sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", gps->sql);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return NULL;
	}
	for (i = 0; i < gps->argc; i++)
		SQLBindParameter(stmt, i + 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(gps->argv[i]), 0, gps->argv[i], 0, NULL);

	return stmt;
}

static int retrieve_file(char *dir, int msgnum)
{
	int x = 0;
	int res;
	int fd = -1;
	size_t fdlen = 0;
	void *fdm = MAP_FAILED;
	SQLSMALLINT colcount = 0;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char fmt[80] = "";
	char *c;
	char coltitle[256];
	SQLSMALLINT collen;
	SQLSMALLINT datatype;
	SQLSMALLINT decimaldigits;
	SQLSMALLINT nullable;
	SQLULEN colsize;
	SQLLEN colsize2;
	FILE *f = NULL;
	char rowdata[80];
	char fn[PATH_MAX];
	char full_fn[PATH_MAX];
	char msgnums[80];
	char *argv[] = { dir, msgnums };
	struct generic_prepare_struct gps = { .sql = sql, .argc = 2, .argv = argv };

	struct odbc_obj *obj;
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		ast_copy_string(fmt, vmfmts, sizeof(fmt));
		c = strchr(fmt, '|');
		if (c)
			*c = '\0';
		if (!strcasecmp(fmt, "wav49"))
			strcpy(fmt, "WAV");
		snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		if (msgnum > -1)
			make_file(fn, sizeof(fn), dir, msgnum);
		else
			ast_copy_string(fn, dir, sizeof(fn));
		snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
		
		if (!(f = fopen(full_fn, "w+"))) {
			ast_log(LOG_WARNING, "Failed to open/create '%s'\n", full_fn);
			goto yuck;
		}
		
		snprintf(full_fn, sizeof(full_fn), "%s.%s", fn, fmt);
		snprintf(sql, sizeof(sql), "SELECT * FROM %s WHERE dir=? AND msgnum=?", odbc_table);
		stmt = ast_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if (res == SQL_NO_DATA) {
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		} else if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		fd = open(full_fn, O_RDWR | O_CREAT | O_TRUNC, VOICEMAIL_FILE_MODE);
		if (fd < 0) {
			ast_log(LOG_WARNING, "Failed to write '%s': %s\n", full_fn, strerror(errno));
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLNumResultCols(stmt, &colcount);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {	
			ast_log(LOG_WARNING, "SQL Column Count error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		if (f) 
			fprintf(f, "[message]\n");
		for (x = 0; x < colcount; x++) {
			rowdata[0] = '\0';
			collen = sizeof(coltitle);
			res = SQLDescribeCol(stmt, x + 1, (unsigned char *)coltitle, sizeof(coltitle), &collen, 
						&datatype, &colsize, &decimaldigits, &nullable);
			if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
				ast_log(LOG_WARNING, "SQL Describe Column error!\n[%s]\n\n", sql);
				SQLFreeHandle (SQL_HANDLE_STMT, stmt);
				ast_odbc_release_obj(obj);
				goto yuck;
			}
			if (!strcasecmp(coltitle, "recording")) {
				off_t offset;
				res = SQLGetData(stmt, x + 1, SQL_BINARY, rowdata, 0, &colsize2);
				fdlen = colsize2;
				if (fd > -1) {
					char tmp[1] = "";
					lseek(fd, fdlen - 1, SEEK_SET);
					if (write(fd, tmp, 1) != 1) {
						close(fd);
						fd = -1;
						continue;
					}
					/* Read out in small chunks */
					for (offset = 0; offset < colsize2; offset += CHUNKSIZE) {
						if ((fdm = mmap(NULL, CHUNKSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, offset)) == MAP_FAILED) {
							ast_log(LOG_WARNING, "Could not mmap the output file: %s (%d)\n", strerror(errno), errno);
							SQLFreeHandle(SQL_HANDLE_STMT, stmt);
							ast_odbc_release_obj(obj);
							goto yuck;
						} else {
							res = SQLGetData(stmt, x + 1, SQL_BINARY, fdm, CHUNKSIZE, NULL);
							munmap(fdm, CHUNKSIZE);
							if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
								ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
								unlink(full_fn);
								SQLFreeHandle(SQL_HANDLE_STMT, stmt);
								ast_odbc_release_obj(obj);
								goto yuck;
							}
						}
					}
					truncate(full_fn, fdlen);
				}
			} else {
				SQLLEN ind;
				res = SQLGetData(stmt, x + 1, SQL_CHAR, rowdata, sizeof(rowdata), &ind);
				if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
					SQLINTEGER nativeerror = 0;
					SQLSMALLINT diagbytes = 0;
					unsigned char state[10], diagnostic[256];
					SQLGetDiagRec(SQL_HANDLE_STMT, stmt, 1, state, &nativeerror, diagnostic, sizeof(diagnostic), &diagbytes);
					ast_log(LOG_WARNING, "SQL Get Data error: %s: %s!\n[%s]\n\n", state, diagnostic, sql);
					SQLFreeHandle (SQL_HANDLE_STMT, stmt);
					ast_odbc_release_obj(obj);
					goto yuck;
				}
				if (strcasecmp(coltitle, "msgnum") && strcasecmp(coltitle, "dir") && f)
					fprintf(f, "%s=%s\n", coltitle, rowdata);
			}
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	if (f)
		fclose(f);
	if (fd > -1)
		close(fd);
	return x - 1;
}

/*!
* \brief Determines the highest message number in use for a given user and mailbox folder.
* \param vmu 
* \param dir the folder the mailbox folder to look for messages. Used to construct the SQL where clause.
*
* This method is used when mailboxes are stored in an ODBC back end.
* Typical use to set the msgnum would be to take the value returned from this method and add one to it.
*
* \return the value of zero or greaterto indicate the last message index in use, -1 to indicate none.
*/
static int last_message_index(struct ast_vm_user *vmu, char *dir)
{
	int x = 0;
	int res;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char rowdata[20];
	char *argv[] = { dir };
	struct generic_prepare_struct gps = { .sql = sql, .argc = 1, .argv = argv };

	struct odbc_obj *obj;
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir=?", odbc_table);
		stmt = ast_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		if (sscanf(rowdata, "%d", &x) != 1)
			ast_log(LOG_WARNING, "Failed to read message count!\n");
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	return x - 1;
}

static int message_exists(char *dir, int msgnum)
{
	int x = 0;
	int res;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char rowdata[20];
	char msgnums[20];
	char *argv[] = { dir, msgnums };
	struct generic_prepare_struct gps = { .sql = sql, .argc = 2, .argv = argv };

	struct odbc_obj *obj;
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir=? AND msgnum=?", odbc_table);
		stmt = ast_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		if (sscanf(rowdata, "%d", &x) != 1)
			ast_log(LOG_WARNING, "Failed to read message count!\n");
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	return x;
}

static int count_messages(struct ast_vm_user *vmu, char *dir)
{
	return last_message_index(vmu, dir) + 1;
}

static void delete_file(char *sdir, int smsg)
{
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char msgnums[20];
	char *argv[] = { sdir, msgnums };
	struct generic_prepare_struct gps = { .sql = sql, .argc = 2, .argv = argv };

	struct odbc_obj *obj;
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", smsg);
		snprintf(sql, sizeof(sql), "DELETE FROM %s WHERE dir=? AND msgnum=?", odbc_table);
		stmt = ast_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt)
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
		else
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
	return;	
}

static void copy_file(char *sdir, int smsg, char *ddir, int dmsg, char *dmailboxuser, char *dmailboxcontext)
{
	SQLHSTMT stmt;
	char sql[512];
	char msgnums[20];
	char msgnumd[20];
	struct odbc_obj *obj;
	char *argv[] = { ddir, msgnumd, dmailboxuser, dmailboxcontext, sdir, msgnums };
	struct generic_prepare_struct gps = { .sql = sql, .argc = 6, .argv = argv };

	delete_file(ddir, dmsg);
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", smsg);
		snprintf(msgnumd, sizeof(msgnumd), "%d", dmsg);
		snprintf(sql, sizeof(sql), "INSERT INTO %s (dir, msgnum, context, macrocontext, callerid, origtime, duration, recording, mailboxuser, mailboxcontext) SELECT ?,?,context,macrocontext,callerid,origtime,duration,recording,?,? FROM %s WHERE dir=? AND msgnum=?", odbc_table, odbc_table);
		stmt = ast_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt)
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s] (You probably don't have MySQL 4.1 or later installed)\n\n", sql);
		else
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
	return;	
}

struct insert_cb_struct {
	char *dir;
	char *msgnum;
	void *recording;
	size_t recordinglen;
	SQLLEN indlen;
	const char *context;
	const char *macrocontext;
	const char *callerid;
	const char *origtime;
	const char *duration;
	char *mailboxuser;
	char *mailboxcontext;
	const char *category;
	char *sql;
};

static SQLHSTMT insert_cb(struct odbc_obj *obj, void *vd)
{
	struct insert_cb_struct *d = vd;
	int res;
	SQLHSTMT stmt;

	res = SQLAllocHandle(SQL_HANDLE_STMT, obj->con, &stmt);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Alloc Handle failed!\n");
		return NULL;
	}

	res = SQLPrepare(stmt, (unsigned char *)d->sql, SQL_NTS);
	if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
		ast_log(LOG_WARNING, "SQL Prepare failed![%s]\n", d->sql);
		SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		return NULL;
	}

	SQLBindParameter(stmt, 1, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->dir), 0, (void *)d->dir, 0, NULL);
	SQLBindParameter(stmt, 2, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->msgnum), 0, (void *)d->msgnum, 0, NULL);
	SQLBindParameter(stmt, 3, SQL_PARAM_INPUT, SQL_C_BINARY, SQL_LONGVARBINARY, d->recordinglen, 0, (void *)d->recording, 0, &d->indlen);
	SQLBindParameter(stmt, 4, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->context), 0, (void *)d->context, 0, NULL);
	SQLBindParameter(stmt, 5, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->macrocontext), 0, (void *)d->macrocontext, 0, NULL);
	SQLBindParameter(stmt, 6, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->callerid), 0, (void *)d->callerid, 0, NULL);
	SQLBindParameter(stmt, 7, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->origtime), 0, (void *)d->origtime, 0, NULL);
	SQLBindParameter(stmt, 8, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->duration), 0, (void *)d->duration, 0, NULL);
	SQLBindParameter(stmt, 9, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->mailboxuser), 0, (void *)d->mailboxuser, 0, NULL);
	SQLBindParameter(stmt, 10, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->mailboxcontext), 0, (void *)d->mailboxcontext, 0, NULL);
	if (!ast_strlen_zero(d->category)) {
		SQLBindParameter(stmt, 11, SQL_PARAM_INPUT, SQL_C_CHAR, SQL_CHAR, strlen(d->category), 0, (void *)d->category, 0, NULL);
	}

	return stmt;
}

static int store_file(char *dir, char *mailboxuser, char *mailboxcontext, int msgnum)
{
	int x = 0;
	int fd = -1;
	void *fdm = MAP_FAILED;
	size_t fdlen = -1;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char msgnums[20];
	char fn[PATH_MAX];
	char full_fn[PATH_MAX];
	char fmt[80] = "";
	char *c;
	struct insert_cb_struct d = {
		.dir = dir,
		.msgnum = msgnums,
		.context = "",
		.macrocontext = "",
		.callerid = "",
		.origtime = "",
		.duration = "",
		.mailboxuser = mailboxuser,
		.mailboxcontext = mailboxcontext,
		.category = "",
		.sql = sql
	};
	struct ast_config *cfg = NULL;
	struct odbc_obj *obj;
	struct ast_flags config_flags = { CONFIG_FLAG_NOCACHE };

	delete_file(dir, msgnum);
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		ast_copy_string(fmt, vmfmts, sizeof(fmt));
		c = strchr(fmt, '|');
		if (c)
			*c = '\0';
		if (!strcasecmp(fmt, "wav49"))
			strcpy(fmt, "WAV");
		snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		if (msgnum > -1)
			make_file(fn, sizeof(fn), dir, msgnum);
		else
			ast_copy_string(fn, dir, sizeof(fn));
		snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
		cfg = ast_config_load(full_fn, config_flags);
		snprintf(full_fn, sizeof(full_fn), "%s.%s", fn, fmt);
		fd = open(full_fn, O_RDWR);
		if (fd < 0) {
			ast_log(LOG_WARNING, "Open of sound file '%s' failed: %s\n", full_fn, strerror(errno));
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		if (cfg) {
			d.context = ast_variable_retrieve(cfg, "message", "context");
			if (!d.context) d.context = "";
			d.macrocontext = ast_variable_retrieve(cfg, "message", "macrocontext");
			if (!d.macrocontext) d.macrocontext = "";
			d.callerid = ast_variable_retrieve(cfg, "message", "callerid");
			if (!d.callerid) d.callerid = "";
			d.origtime = ast_variable_retrieve(cfg, "message", "origtime");
			if (!d.origtime) d.origtime = "";
			d.duration = ast_variable_retrieve(cfg, "message", "duration");
			if (!d.duration) d.duration = "";
			d.category = ast_variable_retrieve(cfg, "message", "category");
			if (!d.category) d.category = "";
		}
		fdlen = lseek(fd, 0, SEEK_END);
		lseek(fd, 0, SEEK_SET);
		printf("Length is %zd\n", fdlen);
		fdm = mmap(NULL, fdlen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (fdm == MAP_FAILED) {
			ast_log(LOG_WARNING, "Memory map failed!\n");
			ast_odbc_release_obj(obj);
			goto yuck;
		} 
		d.recording = fdm;
		d.recordinglen = d.indlen = fdlen; /* SQL_LEN_DATA_AT_EXEC(fdlen); */
		if (!ast_strlen_zero(d.category)) 
			snprintf(sql, sizeof(sql), "INSERT INTO %s (dir,msgnum,recording,context,macrocontext,callerid,origtime,duration,mailboxuser,mailboxcontext,category) VALUES (?,?,?,?,?,?,?,?,?,?,?)", odbc_table);
		else
			snprintf(sql, sizeof(sql), "INSERT INTO %s (dir,msgnum,recording,context,macrocontext,callerid,origtime,duration,mailboxuser,mailboxcontext) VALUES (?,?,?,?,?,?,?,?,?,?)", odbc_table);
		stmt = ast_odbc_prepare_and_execute(obj, insert_cb, &d);
		if (stmt) {
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		}
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
yuck:	
	if (cfg)
		ast_config_destroy(cfg);
	if (fdm != MAP_FAILED)
		munmap(fdm, fdlen);
	if (fd > -1)
		close(fd);
	return x;
}

static void rename_file(char *sdir, int smsg, char *mailboxuser, char *mailboxcontext, char *ddir, int dmsg)
{
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char msgnums[20];
	char msgnumd[20];
	struct odbc_obj *obj;
	char *argv[] = { ddir, msgnumd, mailboxuser, mailboxcontext, sdir, msgnums };
	struct generic_prepare_struct gps = { .sql = sql, .argc = 6, .argv = argv };

	delete_file(ddir, dmsg);
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(msgnums, sizeof(msgnums), "%d", smsg);
		snprintf(msgnumd, sizeof(msgnumd), "%d", dmsg);
		snprintf(sql, sizeof(sql), "UPDATE %s SET dir=?, msgnum=?, mailboxuser=?, mailboxcontext=? WHERE dir=? AND msgnum=?", odbc_table);
		stmt = ast_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt)
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
		else
			SQLFreeHandle(SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
	return;	
}

/*!
 * \brief Removes a voicemail message file.
 * \param dir the path to the message file.
 * \param msgnum the unique number for the message within the mailbox.
 *
 * Removes the message content file and the information file.
 * This method is used by the DISPOSE macro when mailboxes are stored in an ODBC back end.
 * Typical use is to clean up after a RETRIEVE operation. 
 * Note that this does not remove the message from the mailbox folders, to do that we would use delete_file().
 * \return zero on success, -1 on error.
 */
static int remove_file(char *dir, int msgnum)
{
	char fn[PATH_MAX];
	char full_fn[PATH_MAX];
	char msgnums[80];
	
	if (msgnum > -1) {
		snprintf(msgnums, sizeof(msgnums), "%d", msgnum);
		make_file(fn, sizeof(fn), dir, msgnum);
	} else
		ast_copy_string(fn, dir, sizeof(fn));
	ast_filedelete(fn, NULL);	
	snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
	unlink(full_fn);
	return 0;
}
#else
#ifndef IMAP_STORAGE
static int count_messages(struct ast_vm_user *vmu, char *dir)
{
	/* Find all .txt files - even if they are not in sequence from 0000 */

	int vmcount = 0;
	DIR *vmdir = NULL;
	struct dirent *vment = NULL;

	if (vm_lock_path(dir))
		return ERROR_LOCK_PATH;

	if ((vmdir = opendir(dir))) {
		while ((vment = readdir(vmdir))) {
			if (strlen(vment->d_name) > 7 && !strncmp(vment->d_name + 7, ".txt", 4)) 
				vmcount++;
		}
		closedir(vmdir);
	}
	ast_unlock_path(dir);
	
	return vmcount;
}

static void rename_file(char *sfn, char *dfn)
{
	char stxt[PATH_MAX];
	char dtxt[PATH_MAX];
	ast_filerename(sfn, dfn, NULL);
	snprintf(stxt, sizeof(stxt), "%s.txt", sfn);
	snprintf(dtxt, sizeof(dtxt), "%s.txt", dfn);
	if (ast_check_realtime("voicemail_data")) {
		ast_update_realtime("voicemail_data", "filename", sfn, "filename", dfn, NULL);
	}
	rename(stxt, dtxt);
}
#endif

#ifndef IMAP_STORAGE
/*! \brief
* A negative return value indicates an error.
* \note Should always be called with a lock already set on dir.
*/
static int last_message_index(struct ast_vm_user *vmu, char *dir)
{
	int x;
	unsigned char map[MAXMSGLIMIT] = "";
	DIR *msgdir;
	struct dirent *msgdirent;
	int msgdirint;

	/* Reading the entire directory into a file map scales better than
	* doing a stat repeatedly on a predicted sequence.  I suspect this
	* is partially due to stat(2) internally doing a readdir(2) itself to
	* find each file. */
	msgdir = opendir(dir);
	while ((msgdirent = readdir(msgdir))) {
		if (sscanf(msgdirent->d_name, "msg%d", &msgdirint) == 1 && msgdirint < MAXMSGLIMIT)
			map[msgdirint] = 1;
	}
	closedir(msgdir);

	for (x = 0; x < vmu->maxmsg; x++) {
		if (map[x] == 0)
			break;
	}

	return x - 1;
}

#endif /* #ifndef IMAP_STORAGE */
#endif /* #else of #ifdef ODBC_STORAGE */

static int copy(char *infile, char *outfile)
{
	int ifd;
	int ofd;
	int res;
	int len;
	char buf[4096];

#ifdef HARDLINK_WHEN_POSSIBLE
	/* Hard link if possible; saves disk space & is faster */
	if (link(infile, outfile)) {
#endif
		if ((ifd = open(infile, O_RDONLY)) < 0) {
			ast_log(LOG_WARNING, "Unable to open %s in read-only mode: %s\n", infile, strerror(errno));
			return -1;
		}
		if ((ofd = open(outfile, O_WRONLY | O_TRUNC | O_CREAT, VOICEMAIL_FILE_MODE)) < 0) {
			ast_log(LOG_WARNING, "Unable to open %s in write-only mode: %s\n", outfile, strerror(errno));
			close(ifd);
			return -1;
		}
		do {
			len = read(ifd, buf, sizeof(buf));
			if (len < 0) {
				ast_log(LOG_WARNING, "Read failed on %s: %s\n", infile, strerror(errno));
				close(ifd);
				close(ofd);
				unlink(outfile);
			}
			if (len) {
				res = write(ofd, buf, len);
				if (errno == ENOMEM || errno == ENOSPC || res != len) {
					ast_log(LOG_WARNING, "Write failed on %s (%d of %d): %s\n", outfile, res, len, strerror(errno));
					close(ifd);
					close(ofd);
					unlink(outfile);
				}
			}
		} while (len);
		close(ifd);
		close(ofd);
		return 0;
#ifdef HARDLINK_WHEN_POSSIBLE
	} else {
		/* Hard link succeeded */
		return 0;
	}
#endif
}

static void copy_plain_file(char *frompath, char *topath)
{
	char frompath2[PATH_MAX], topath2[PATH_MAX];
	struct ast_variable *tmp, *var = NULL;
	const char *origmailbox = NULL, *context = NULL, *macrocontext = NULL, *exten = NULL, *priority = NULL, *callerchan = NULL, *callerid = NULL, *origdate = NULL, *origtime = NULL, *category = NULL, *duration = NULL;
	ast_filecopy(frompath, topath, NULL);
	snprintf(frompath2, sizeof(frompath2), "%s.txt", frompath);
	snprintf(topath2, sizeof(topath2), "%s.txt", topath);
	if (ast_check_realtime("voicemail_data")) {
		var = ast_load_realtime("voicemail_data", "filename", frompath, NULL);
		/* This cycle converts ast_variable linked list, to va_list list of arguments, may be there is a better way to do it? */
		for (tmp = var; tmp; tmp = tmp->next) {
			if (!strcasecmp(tmp->name, "origmailbox")) {
				origmailbox = tmp->value;
			} else if (!strcasecmp(tmp->name, "context")) {
				context = tmp->value;
			} else if (!strcasecmp(tmp->name, "macrocontext")) {
				macrocontext = tmp->value;
			} else if (!strcasecmp(tmp->name, "exten")) {
				exten = tmp->value;
			} else if (!strcasecmp(tmp->name, "priority")) {
				priority = tmp->value;
			} else if (!strcasecmp(tmp->name, "callerchan")) {
				callerchan = tmp->value;
			} else if (!strcasecmp(tmp->name, "callerid")) {
				callerid = tmp->value;
			} else if (!strcasecmp(tmp->name, "origdate")) {
				origdate = tmp->value;
			} else if (!strcasecmp(tmp->name, "origtime")) {
				origtime = tmp->value;
			} else if (!strcasecmp(tmp->name, "category")) {
				category = tmp->value;
			} else if (!strcasecmp(tmp->name, "duration")) {
				duration = tmp->value;
			}
		}
		ast_store_realtime("voicemail_data", "filename", topath, "origmailbox", origmailbox, "context", context, "macrocontext", macrocontext, "exten", exten, "priority", priority, "callerchan", callerchan, "callerid", callerid, "origdate", origdate, "origtime", origtime, "category", category, "duration", duration, NULL);
	}
	copy(frompath2, topath2);
	ast_variables_destroy(var);
}


#if (!defined(ODBC_STORAGE) && !defined(IMAP_STORAGE))
/*! 
* \brief Removes the voicemail sound and information file.
* \param file The path to the sound file. This will be the the folder and message index, without the extension.
*
* This is used by the DELETE macro when voicemails are stored on the file system.
*
* \return zero on success, -1 on error.
*/
static int vm_delete(char *file)
{
	char *txt;
	int txtsize = 0;

	txtsize = (strlen(file) + 5) * sizeof(char);
	txt = alloca(txtsize);
	/* Sprintf here would safe because we alloca'd exactly the right length,
	* but trying to eliminate all sprintf's anyhow
	*/
	if (ast_check_realtime("voicemail_data")) {
		ast_destroy_realtime("voicemail_data", "filename", file, NULL);
	}
	snprintf(txt, txtsize, "%s.txt", file);
	unlink(txt);
	return ast_filedelete(file, NULL);
}
#endif
static int inbuf(struct baseio *bio, FILE *fi)
{
	int l;

	if (bio->ateof)
		return 0;

	if ((l = fread(bio->iobuf, 1, BASEMAXINLINE, fi)) <= 0) {
		if (ferror(fi))
			return -1;

		bio->ateof = 1;
		return 0;
	}

	bio->iolen = l;
	bio->iocp = 0;

	return 1;
}

static int inchar(struct baseio *bio, FILE *fi)
{
	if (bio->iocp >= bio->iolen) {
		if (!inbuf(bio, fi))
			return EOF;
	}

	return bio->iobuf[bio->iocp++];
}

static int ochar(struct baseio *bio, int c, FILE *so)
{
	if (bio->linelength >= BASELINELEN) {
		if (fputs(eol, so) == EOF)
			return -1;

		bio->linelength= 0;
	}

	if (putc(((unsigned char)c), so) == EOF)
		return -1;

	bio->linelength++;

	return 1;
}

static int base_encode(char *filename, FILE *so)
{
	static const unsigned char dtable[] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',
		'L', 'M', 'N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
		'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z', '0',
		'1', '2', '3', '4', '5', '6', '7', '8', '9', '+', '/'};
	int i, hiteof = 0;
	FILE *fi;
	struct baseio bio;

	memset(&bio, 0, sizeof(bio));
	bio.iocp = BASEMAXINLINE;

	if (!(fi = fopen(filename, "rb"))) {
		ast_log(LOG_WARNING, "Failed to open file: %s: %s\n", filename, strerror(errno));
		return -1;
	}

	while (!hiteof) {
		unsigned char igroup[3], ogroup[4];
		int c, n;

		igroup[0] = igroup[1] = igroup[2] = 0;

		for (n = 0; n < 3; n++) {
			if ((c = inchar(&bio, fi)) == EOF) {
				hiteof = 1;
				break;
			}

			igroup[n] = (unsigned char)c;
		}

		if (n > 0) {
			ogroup[0] = dtable[igroup[0] >> 2];
			ogroup[1] = dtable[((igroup[0] & 3) << 4) | (igroup[1] >> 4)];
			ogroup[2] = dtable[((igroup[1] & 0xF) << 2) | (igroup[2] >> 6)];
			ogroup[3] = dtable[igroup[2] & 0x3F];

			if (n < 3) {
				ogroup[3] = '=';

				if (n < 2)
					ogroup[2] = '=';
			}

			for (i = 0; i < 4; i++)
				ochar(&bio, ogroup[i], so);
		}
	}

	fclose(fi);
	
	if (fputs(eol, so) == EOF)
		return 0;

	return 1;
}

static void prep_email_sub_vars(struct ast_channel *ast, struct ast_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *dur, char *date, char *passdata, size_t passdatasize, const char *category)
{
	char callerid[256];
	/* Prepare variables for substitution in email body and subject */
	pbx_builtin_setvar_helper(ast, "VM_NAME", vmu->fullname);
	pbx_builtin_setvar_helper(ast, "VM_DUR", dur);
	snprintf(passdata, passdatasize, "%d", msgnum);
	pbx_builtin_setvar_helper(ast, "VM_MSGNUM", passdata);
	pbx_builtin_setvar_helper(ast, "VM_CONTEXT", context);
	pbx_builtin_setvar_helper(ast, "VM_MAILBOX", mailbox);
	pbx_builtin_setvar_helper(ast, "VM_CALLERID", ast_callerid_merge(callerid, sizeof(callerid), cidname, cidnum, "Unknown Caller"));
	pbx_builtin_setvar_helper(ast, "VM_CIDNAME", (cidname ? cidname : "an unknown caller"));
	pbx_builtin_setvar_helper(ast, "VM_CIDNUM", (cidnum ? cidnum : "an unknown caller"));
	pbx_builtin_setvar_helper(ast, "VM_DATE", date);
	pbx_builtin_setvar_helper(ast, "VM_CATEGORY", category ? ast_strdupa(category) : "no category");
}

static char *quote(const char *from, char *to, size_t len)
{
	char *ptr = to;
	*ptr++ = '"';
	for (; ptr < to + len - 1; from++) {
		if (*from == '"')
			*ptr++ = '\\';
		else if (*from == '\0')
			break;
		*ptr++ = *from;
	}
	if (ptr < to + len - 1)
		*ptr++ = '"';
	*ptr = '\0';
	return to;
}

/*! \brief
* fill in *tm for current time according to the proper timezone, if any.
* Return tm so it can be used as a function argument.
*/
static const struct ast_tm *vmu_tm(const struct ast_vm_user *vmu, struct ast_tm *tm)
{
	const struct vm_zone *z = NULL;
	struct timeval t = ast_tvnow();

	/* Does this user have a timezone specified? */
	if (!ast_strlen_zero(vmu->zonetag)) {
		/* Find the zone in the list */
		AST_LIST_LOCK(&zones);
		AST_LIST_TRAVERSE(&zones, z, list) {
			if (!strcmp(z->name, vmu->zonetag))
				break;
		}
		AST_LIST_UNLOCK(&zones);
	}
	ast_localtime(&t, tm, z ? z->timezone : NULL);
	return tm;
}

static void make_email_file(FILE *p, char *srcemail, struct ast_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *attach, char *format, int duration, int attach_user_voicemail, struct ast_channel *chan, const char *category, int imap)
{
	char date[256];
	char host[MAXHOSTNAMELEN] = "";
	char who[256];
	char bound[256];
	char fname[256];
	char dur[256];
	char tmpcmd[256];
	struct ast_tm tm;
	char enc_cidnum[256] = "", enc_cidname[256] = "";
	char *passdata2;
	size_t len_passdata;
	char *greeting_attachment;

#ifdef IMAP_STORAGE
#define ENDL "\r\n"
#else
#define ENDL "\n"
#endif

	if (!ast_strlen_zero(cidnum)) {
		strip_control(cidnum, enc_cidnum, sizeof(enc_cidnum));
	}
	if (!ast_strlen_zero(cidname)) {
		strip_control(cidname, enc_cidname, sizeof(enc_cidname));
	}
	gethostname(host, sizeof(host) - 1);

	if (strchr(srcemail, '@'))
		ast_copy_string(who, srcemail, sizeof(who));
	else 
		snprintf(who, sizeof(who), "%s@%s", srcemail, host);
	
	greeting_attachment = strrchr(ast_strdupa(attach), '/');
	if (greeting_attachment)
		*greeting_attachment++ = '\0';

	snprintf(dur, sizeof(dur), "%d:%02d", duration / 60, duration % 60);
	ast_strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S %z", vmu_tm(vmu, &tm));
	fprintf(p, "Date: %s" ENDL, date);

	/* Set date format for voicemail mail */
	ast_strftime(date, sizeof(date), emaildateformat, &tm);

	if (!ast_strlen_zero(fromstring)) {
		struct ast_channel *ast;
		if ((ast = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, "", "", "", 0, 0))) {
			char *passdata;
			int vmlen = strlen(fromstring) * 3 + 200;
			passdata = alloca(vmlen);
			memset(passdata, 0, vmlen);
			prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, enc_cidnum, enc_cidname, dur, date, passdata, vmlen, category);
			pbx_substitute_variables_helper(ast, fromstring, passdata, vmlen);
			len_passdata = strlen(passdata) * 2 + 3;
			passdata2 = alloca(len_passdata);
			fprintf(p, "From: %s <%s>" ENDL, quote(passdata, passdata2, len_passdata), who);
			ast_channel_free(ast);
		} else {
			ast_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
		}
	} else {
		fprintf(p, "From: Asterisk PBX <%s>" ENDL, who);
	}
	len_passdata = strlen(vmu->fullname) * 2 + 3;
	passdata2 = alloca(len_passdata);
	fprintf(p, "To: %s <%s>" ENDL, quote(vmu->fullname, passdata2, len_passdata), vmu->email);
	if (!ast_strlen_zero(emailsubject)) {
		struct ast_channel *ast;
		if ((ast = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, "", "", "", 0, 0))) {
			char *passdata;
			int vmlen = strlen(emailsubject) * 3 + 200;
			passdata = alloca(vmlen);
			memset(passdata, 0, vmlen);
			prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, enc_cidnum, enc_cidname, dur, date, passdata, vmlen, category);
			pbx_substitute_variables_helper(ast, emailsubject, passdata, vmlen);
			fprintf(p, "Subject: %s" ENDL, passdata);
			ast_channel_free(ast);
		} else {
			ast_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
		}
	} else if (ast_test_flag((&globalflags), VM_PBXSKIP)) {
		fprintf(p, "Subject: New message %d in mailbox %s" ENDL, msgnum + 1, mailbox);
	} else {
		fprintf(p, "Subject: [PBX]: New message %d in mailbox %s" ENDL, msgnum + 1, mailbox);
	}

	fprintf(p, "Message-ID: <Asterisk-%d-%d-%s-%d@%s>" ENDL, msgnum + 1, (unsigned int)ast_random(), mailbox, (int)getpid(), host);
	if (imap) {
		/* additional information needed for IMAP searching */
		fprintf(p, "X-Asterisk-VM-Message-Num: %d" ENDL, msgnum + 1);
		/* fprintf(p, "X-Asterisk-VM-Orig-Mailbox: %s" ENDL, ext); */
		fprintf(p, "X-Asterisk-VM-Server-Name: %s" ENDL, fromstring);
		fprintf(p, "X-Asterisk-VM-Context: %s" ENDL, context);
		fprintf(p, "X-Asterisk-VM-Extension: %s" ENDL, mailbox);
		fprintf(p, "X-Asterisk-VM-Priority: %d" ENDL, chan->priority);
		fprintf(p, "X-Asterisk-VM-Caller-channel: %s" ENDL, chan->name);
		fprintf(p, "X-Asterisk-VM-Caller-ID-Num: %s" ENDL, enc_cidnum);
		fprintf(p, "X-Asterisk-VM-Caller-ID-Name: %s" ENDL, enc_cidname);
		fprintf(p, "X-Asterisk-VM-Duration: %d" ENDL, duration);
		if (!ast_strlen_zero(category)) {
			fprintf(p, "X-Asterisk-VM-Category: %s" ENDL, category);
		} else {
			fprintf(p, "X-Asterisk-VM-Category: " ENDL);
		}
		fprintf(p, "X-Asterisk-VM-Message-Type: %s" ENDL, msgnum > -1 ? "Message" : greeting_attachment);
		fprintf(p, "X-Asterisk-VM-Orig-date: %s" ENDL, date);
		fprintf(p, "X-Asterisk-VM-Orig-time: %ld" ENDL, (long)time(NULL));
	}
	if (!ast_strlen_zero(cidnum)) {
		fprintf(p, "X-Asterisk-CallerID: %s" ENDL, enc_cidnum);
	}
	if (!ast_strlen_zero(cidname)) {
		fprintf(p, "X-Asterisk-CallerIDName: %s" ENDL, enc_cidname);
	}
	fprintf(p, "MIME-Version: 1.0" ENDL);
	if (attach_user_voicemail) {
		/* Something unique. */
		snprintf(bound, sizeof(bound), "----voicemail_%d%s%d%d", msgnum + 1, mailbox, (int)getpid(), (unsigned int)ast_random());

		fprintf(p, "Content-Type: multipart/mixed; boundary=\"%s\"" ENDL, bound);
		fprintf(p, ENDL ENDL "This is a multi-part message in MIME format." ENDL ENDL);
		fprintf(p, "--%s" ENDL, bound);
	}
	fprintf(p, "Content-Type: text/plain; charset=%s" ENDL "Content-Transfer-Encoding: 8bit" ENDL ENDL, charset);
	if (emailbody) {
		struct ast_channel *ast;
		if ((ast = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, "", "", "", 0, 0))) {
			char *passdata;
			int vmlen = strlen(emailbody) * 3 + 200;
			passdata = alloca(vmlen);
			memset(passdata, 0, vmlen);
			prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
			pbx_substitute_variables_helper(ast, emailbody, passdata, vmlen);
			fprintf(p, "%s" ENDL, passdata);
			ast_channel_free(ast);
		} else
			ast_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else if (msgnum > -1) {
		fprintf(p, "Dear %s:" ENDL ENDL "\tJust wanted to let you know you were just left a %s long message (number %d)" ENDL

		"in mailbox %s from %s, on %s so you might" ENDL
		"want to check it when you get a chance.  Thanks!" ENDL ENDL "\t\t\t\t--Asterisk" ENDL ENDL, vmu->fullname, 
		dur, msgnum + 1, mailbox, (cidname ? cidname : (cidnum ? cidnum : "an unknown caller")), date);
	} else {
		fprintf(p, "This message is to let you know that your greeting was changed on %s." ENDL
				"Please do not delete this message, lest your greeting vanish with it." ENDL ENDL, date);
	}
	if (attach_user_voicemail) {
		/* Eww. We want formats to tell us their own MIME type */
		char *ctype = (!strcasecmp(format, "ogg")) ? "application/" : "audio/x-";
		char tmpdir[256], newtmp[256];
		int tmpfd = -1;
	
		if (vmu->volgain < -.001 || vmu->volgain > .001) {
			create_dirpath(tmpdir, sizeof(tmpdir), vmu->context, vmu->mailbox, "tmp");
			snprintf(newtmp, sizeof(newtmp), "%s/XXXXXX", tmpdir);
			tmpfd = mkstemp(newtmp);
			chmod(newtmp, VOICEMAIL_FILE_MODE & ~my_umask);
			ast_debug(3, "newtmp: %s\n", newtmp);
			if (tmpfd > -1) {
				int soxstatus;
				snprintf(tmpcmd, sizeof(tmpcmd), "sox -v %.4f %s.%s %s.%s", vmu->volgain, attach, format, newtmp, format);
				if ((soxstatus = ast_safe_system(tmpcmd)) == 0) {
					attach = newtmp;
					ast_debug(3, "VOLGAIN: Stored at: %s.%s - Level: %.4f - Mailbox: %s\n", attach, format, vmu->volgain, mailbox);
				} else {
					ast_log(LOG_WARNING, "Sox failed to reencode %s.%s: %s (have you installed support for all sox file formats?)\n", attach, format,
						soxstatus == 1 ? "Problem with command line options" : "An error occurred during file processing");
					ast_log(LOG_WARNING, "Voicemail attachment will have no volume gain.\n");
				}
			}
		}
		fprintf(p, "--%s" ENDL, bound);
		if (msgnum > -1)
			fprintf(p, "Content-Type: %s%s; name=\"msg%04d.%s\"" ENDL, ctype, format, msgnum + 1, format);
		else
			fprintf(p, "Content-Type: %s%s; name=\"%s.%s\"" ENDL, ctype, format, greeting_attachment, format);
		fprintf(p, "Content-Transfer-Encoding: base64" ENDL);
		fprintf(p, "Content-Description: Voicemail sound attachment." ENDL);
		if (msgnum > -1)
			fprintf(p, "Content-Disposition: attachment; filename=\"msg%04d.%s\"" ENDL ENDL, msgnum + 1, format);
		else
			fprintf(p, "Content-Disposition: attachment; filename=\"%s.%s\"" ENDL ENDL, greeting_attachment, format);
		snprintf(fname, sizeof(fname), "%s.%s", attach, format);
		base_encode(fname, p);
		fprintf(p, ENDL "--%s--" ENDL "." ENDL, bound);
		if (tmpfd > -1) {
			unlink(fname);
			close(tmpfd);
			unlink(newtmp);
		}
	}
#undef ENDL
}

static int sendmail(char *srcemail, struct ast_vm_user *vmu, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, char *attach, char *format, int duration, int attach_user_voicemail, struct ast_channel *chan, const char *category)
{
	FILE *p = NULL;
	char tmp[80] = "/tmp/astmail-XXXXXX";
	char tmp2[256];

	if (vmu && ast_strlen_zero(vmu->email)) {
		ast_log(LOG_WARNING, "E-mail address missing for mailbox [%s].  E-mail will not be sent.\n", vmu->mailbox);
		return(0);
	}
	if (!strcmp(format, "wav49"))
		format = "WAV";
	ast_debug(3, "Attaching file '%s', format '%s', uservm is '%d', global is %d\n", attach, format, attach_user_voicemail, ast_test_flag((&globalflags), VM_ATTACH));
	/* Make a temporary file instead of piping directly to sendmail, in case the mail
	command hangs */
	if ((p = vm_mkftemp(tmp)) == NULL) {
		ast_log(LOG_WARNING, "Unable to launch '%s' (can't create temporary file)\n", mailcmd);
		return -1;
	} else {
		make_email_file(p, srcemail, vmu, msgnum, context, mailbox, cidnum, cidname, attach, format, duration, attach_user_voicemail, chan, category, 0);
		fclose(p);
		snprintf(tmp2, sizeof(tmp2), "( %s < %s ; rm -f %s ) &", mailcmd, tmp, tmp);
		ast_safe_system(tmp2);
		ast_debug(1, "Sent mail to %s with command '%s'\n", vmu->email, mailcmd);
	}
	return 0;
}

static int sendpage(char *srcemail, char *pager, int msgnum, char *context, char *mailbox, char *cidnum, char *cidname, int duration, struct ast_vm_user *vmu, const char *category)
{
	char date[256];
	char host[MAXHOSTNAMELEN] = "";
	char who[256];
	char dur[PATH_MAX];
	char tmp[80] = "/tmp/astmail-XXXXXX";
	char tmp2[PATH_MAX];
	struct ast_tm tm;
	FILE *p;

	if ((p = vm_mkftemp(tmp)) == NULL) {
		ast_log(LOG_WARNING, "Unable to launch '%s' (can't create temporary file)\n", mailcmd);
		return -1;
	}
	gethostname(host, sizeof(host) - 1);
	if (strchr(srcemail, '@'))
		ast_copy_string(who, srcemail, sizeof(who));
	else 
		snprintf(who, sizeof(who), "%s@%s", srcemail, host);
	snprintf(dur, sizeof(dur), "%d:%02d", duration / 60, duration % 60);
	ast_strftime(date, sizeof(date), "%a, %d %b %Y %H:%M:%S %z", vmu_tm(vmu, &tm));
	fprintf(p, "Date: %s\n", date);

	if (*pagerfromstring) {
		struct ast_channel *ast;
		if ((ast = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, "", "", "", 0, 0))) {
			char *passdata;
			int vmlen = strlen(fromstring) * 3 + 200;
			passdata = alloca(vmlen);
			memset(passdata, 0, vmlen);
			prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
			pbx_substitute_variables_helper(ast, pagerfromstring, passdata, vmlen);
			fprintf(p, "From: %s <%s>\n", passdata, who);
			ast_channel_free(ast);
		} else 
			ast_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else
		fprintf(p, "From: Asterisk PBX <%s>\n", who);
	fprintf(p, "To: %s\n", pager);
	if (pagersubject) {
		struct ast_channel *ast;
		if ((ast = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, "", "", "", 0, 0))) {
			char *passdata;
			int vmlen = strlen(pagersubject) * 3 + 200;
			passdata = alloca(vmlen);
			memset(passdata, 0, vmlen);
			prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
			pbx_substitute_variables_helper(ast, pagersubject, passdata, vmlen);
			fprintf(p, "Subject: %s\n\n", passdata);
			ast_channel_free(ast);
		} else
			ast_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else
		fprintf(p, "Subject: New VM\n\n");

	ast_strftime(date, sizeof(date), "%A, %B %d, %Y at %r", &tm);
	if (pagerbody) {
		struct ast_channel *ast;
		if ((ast = ast_channel_alloc(0, AST_STATE_DOWN, 0, 0, "", "", "", 0, 0))) {
			char *passdata;
			int vmlen = strlen(pagerbody) * 3 + 200;
			passdata = alloca(vmlen);
			memset(passdata, 0, vmlen);
			prep_email_sub_vars(ast, vmu, msgnum + 1, context, mailbox, cidnum, cidname, dur, date, passdata, vmlen, category);
			pbx_substitute_variables_helper(ast, pagerbody, passdata, vmlen);
			fprintf(p, "%s\n", passdata);
			ast_channel_free(ast);
		} else
			ast_log(LOG_WARNING, "Cannot allocate the channel for variables substitution\n");
	} else {
		fprintf(p, "New %s long msg in box %s\n"
				"from %s, on %s", dur, mailbox, (cidname ? cidname : (cidnum ? cidnum : "unknown")), date);
	}
	fclose(p);
	snprintf(tmp2, sizeof(tmp2), "( %s < %s ; rm -f %s ) &", mailcmd, tmp, tmp);
	ast_safe_system(tmp2);
	ast_debug(1, "Sent page to %s with command '%s'\n", pager, mailcmd);
	return 0;
}

static int get_date(char *s, int len)
{
	struct ast_tm tm;
	struct timeval t = ast_tvnow();
	
	ast_localtime(&t, &tm, "UTC");

	return ast_strftime(s, len, "%a %b %e %r UTC %Y", &tm);
}

static int play_greeting(struct ast_channel *chan, struct ast_vm_user *vmu, char *filename, char *ecodes)
{
	int res = -2;
	
#ifdef ODBC_STORAGE
	int success = 
#endif
	RETRIEVE(filename, -1, vmu->mailbox, vmu->context);
	if (ast_fileexists(filename, NULL, NULL) > 0) {
		res = ast_streamfile(chan, filename, chan->language);
		if (res > -1) 
			res = ast_waitstream(chan, ecodes);
#ifdef ODBC_STORAGE
		if (success == -1) {
			/* We couldn't retrieve the file from the database, but we found it on the file system. Let's put it in the database. */
			ast_debug(1, "Greeting not retrieved from database, but found in file storage. Inserting into database\n");
			store_file(filename, vmu->mailbox, vmu->context, -1);
		}
#endif
	}
	DISPOSE(filename, -1);

	return res;
}

static int invent_message(struct ast_channel *chan, struct ast_vm_user *vmu, char *ext, int busy, char *ecodes)
{
	int res;
	char fn[PATH_MAX];
	char dest[PATH_MAX];

	snprintf(fn, sizeof(fn), "%s%s/%s/greet", VM_SPOOL_DIR, vmu->context, ext);

	if ((res = create_dirpath(dest, sizeof(dest), vmu->context, ext, ""))) {
		ast_log(LOG_WARNING, "Failed to make directory(%s)\n", fn);
		return -1;
	}

	res = play_greeting(chan, vmu, fn, ecodes);
	if (res == -2) {
		/* File did not exist */
		res = ast_stream_and_wait(chan, "vm-theperson", ecodes);
		if (res)
			return res;
		res = ast_say_digit_str(chan, ext, ecodes, chan->language);
	}
	if (res)
		return res;

	res = ast_stream_and_wait(chan, busy ? "vm-isonphone" : "vm-isunavail", ecodes);
	return res;
}

static void free_zone(struct vm_zone *z)
{
	ast_free(z);
}

#ifdef ODBC_STORAGE
/*! XXX \todo Fix this function to support multiple mailboxes in the intput string */
static int inboxcount(const char *mailbox, int *newmsgs, int *oldmsgs)
{
	int x = -1;
	int res;
	SQLHSTMT stmt;
	char sql[PATH_MAX];
	char rowdata[20];
	char tmp[PATH_MAX] = "";
	struct odbc_obj *obj;
	char *context;
	struct generic_prepare_struct gps = { .sql = sql, .argc = 0 };

	if (newmsgs)
		*newmsgs = 0;
	if (oldmsgs)
		*oldmsgs = 0;

	/* If no mailbox, return immediately */
	if (ast_strlen_zero(mailbox))
		return 0;

	ast_copy_string(tmp, mailbox, sizeof(tmp));
	
	context = strchr(tmp, '@');
	if (context) {
		*context = '\0';
		context++;
	} else
		context = "default";
	
	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir = '%s%s/%s/%s'", odbc_table, VM_SPOOL_DIR, context, tmp, "INBOX");
		stmt = ast_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		*newmsgs = atoi(rowdata);
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);

		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir = '%s%s/%s/%s'", odbc_table, VM_SPOOL_DIR, context, tmp, "Old");
		stmt = ast_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			ast_odbc_release_obj(obj);
			goto yuck;
		}
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
		ast_odbc_release_obj(obj);
		*oldmsgs = atoi(rowdata);
		x = 0;
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);
		
yuck:	
	return x;
}

static int messagecount(const char *context, const char *mailbox, const char *folder)
{
	struct odbc_obj *obj = NULL;
	int nummsgs = 0;
	int res;
	SQLHSTMT stmt = NULL;
	char sql[PATH_MAX];
	char rowdata[20];
	struct generic_prepare_struct gps = { .sql = sql, .argc = 0 };
	if (!folder)
		folder = "INBOX";
	/* If no mailbox, return immediately */
	if (ast_strlen_zero(mailbox))
		return 0;

	obj = ast_odbc_request_obj(odbc_database, 0);
	if (obj) {
		snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE dir = '%s%s/%s/%s'", odbc_table, VM_SPOOL_DIR, context, mailbox, folder);
		stmt = ast_odbc_prepare_and_execute(obj, generic_prepare, &gps);
		if (!stmt) {
			ast_log(LOG_WARNING, "SQL Execute error!\n[%s]\n\n", sql);
			goto yuck;
		}
		res = SQLFetch(stmt);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Fetch error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		res = SQLGetData(stmt, 1, SQL_CHAR, rowdata, sizeof(rowdata), NULL);
		if ((res != SQL_SUCCESS) && (res != SQL_SUCCESS_WITH_INFO)) {
			ast_log(LOG_WARNING, "SQL Get Data error!\n[%s]\n\n", sql);
			SQLFreeHandle (SQL_HANDLE_STMT, stmt);
			goto yuck;
		}
		nummsgs = atoi(rowdata);
		SQLFreeHandle (SQL_HANDLE_STMT, stmt);
	} else
		ast_log(LOG_WARNING, "Failed to obtain database object for '%s'!\n", odbc_database);

yuck:
	if (obj)
		ast_odbc_release_obj(obj);
	return nummsgs;
}

static int has_voicemail(const char *mailbox, const char *folder)
{
	char tmp[256], *tmp2 = tmp, *mbox, *context;
	ast_copy_string(tmp, mailbox, sizeof(tmp));
	while ((context = mbox = strsep(&tmp2, ","))) {
		strsep(&context, "@");
		if (ast_strlen_zero(context))
			context = "default";
		if (messagecount(context, mbox, folder))
			return 1;
	}
	return 0;
}

#elif defined(IMAP_STORAGE)

#endif
#ifndef IMAP_STORAGE
	/* copy message only used by file storage */
	static int copy_message(struct ast_channel *chan, struct ast_vm_user *vmu, int imbox, int msgnum, long duration, struct ast_vm_user *recip, char *fmt, char *dir)
	{
		char fromdir[PATH_MAX], todir[PATH_MAX], frompath[PATH_MAX], topath[PATH_MAX];
		const char *frombox = mbox(imbox);
		int recipmsgnum;

		ast_log(LOG_NOTICE, "Copying message from %s@%s to %s@%s\n", vmu->mailbox, vmu->context, recip->mailbox, recip->context);

		create_dirpath(todir, sizeof(todir), recip->context, recip->mailbox, "INBOX");
		
		if (!dir)
			make_dir(fromdir, sizeof(fromdir), vmu->context, vmu->mailbox, frombox);
		else
			ast_copy_string(fromdir, dir, sizeof(fromdir));

		make_file(frompath, sizeof(frompath), fromdir, msgnum);
		make_dir(todir, sizeof(todir), recip->context, recip->mailbox, "INBOX");

		if (vm_lock_path(todir))
			return ERROR_LOCK_PATH;

		recipmsgnum = last_message_index(recip, todir) + 1;
		if (recipmsgnum < recip->maxmsg) {
			make_file(topath, sizeof(topath), todir, recipmsgnum);
			COPY(fromdir, msgnum, todir, recipmsgnum, recip->mailbox, recip->context, frompath, topath);
		} else {
			ast_log(LOG_ERROR, "Recipient mailbox %s@%s is full\n", recip->mailbox, recip->context);
		}
		ast_unlock_path(todir);
		notify_new_message(chan, recip, NULL, recipmsgnum, duration, fmt, S_OR(chan->cid.cid_num, NULL), S_OR(chan->cid.cid_name, NULL));
		
		return 0;
	}
#endif
#if !(defined(IMAP_STORAGE) || defined(ODBC_STORAGE))
	static int messagecount(const char *context, const char *mailbox, const char *folder)
	{
		return __has_voicemail(context, mailbox, folder, 0);
	}


	static int __has_voicemail(const char *context, const char *mailbox, const char *folder, int shortcircuit)
	{
		DIR *dir;
		struct dirent *de;
		char fn[256];
		int ret = 0;

		/* If no mailbox, return immediately */
		if (ast_strlen_zero(mailbox))
			return 0;

		if (ast_strlen_zero(folder))
			folder = "INBOX";
		if (ast_strlen_zero(context))
			context = "default";

		snprintf(fn, sizeof(fn), "%s%s/%s/%s", VM_SPOOL_DIR, context, mailbox, folder);

		if (!(dir = opendir(fn)))
			return 0;

		while ((de = readdir(dir))) {
			if (!strncasecmp(de->d_name, "msg", 3)) {
				if (shortcircuit) {
					ret = 1;
					break;
				} else if (!strncasecmp(de->d_name + 8, "txt", 3))
					ret++;
			}
		}

		closedir(dir);

		return ret;
	}


	static int has_voicemail(const char *mailbox, const char *folder)
	{
		char tmp[256], *tmp2 = tmp, *mbox, *context;
		ast_copy_string(tmp, mailbox, sizeof(tmp));
		while ((mbox = strsep(&tmp2, ","))) {
			if ((context = strchr(mbox, '@')))
				*context++ = '\0';
			else
				context = "default";
			if (__has_voicemail(context, mbox, folder, 1))
				return 1;
		}
		return 0;
	}


	static int inboxcount(const char *mailbox, int *newmsgs, int *oldmsgs)
	{
		char tmp[256];
		char *context;

		/* If no mailbox, return immediately */
		if (ast_strlen_zero(mailbox))
			return 0;

		if (newmsgs)
			*newmsgs = 0;
		if (oldmsgs)
			*oldmsgs = 0;

		if (strchr(mailbox, ',')) {
			int tmpnew, tmpold;
			char *mb, *cur;

			ast_copy_string(tmp, mailbox, sizeof(tmp));
			mb = tmp;
			while ((cur = strsep(&mb, ", "))) {
				if (!ast_strlen_zero(cur)) {
					if (inboxcount(cur, newmsgs ? &tmpnew : NULL, oldmsgs ? &tmpold : NULL))
						return -1;
					else {
						if (newmsgs)
							*newmsgs += tmpnew; 
						if (oldmsgs)
							*oldmsgs += tmpold;
					}
				}
			}
			return 0;
		}

		ast_copy_string(tmp, mailbox, sizeof(tmp));
		
		if ((context = strchr(tmp, '@')))
			*context++ = '\0';
		else
			context = "default";

		if (newmsgs)
			*newmsgs = __has_voicemail(context, tmp, "INBOX", 0);
		if (oldmsgs)
			*oldmsgs = __has_voicemail(context, tmp, "Old", 0);

		return 0;
	}

#endif

	static void run_externnotify(char *context, char *extension)
	{
		char arguments[255];
		char ext_context[256] = "";
		int newvoicemails = 0, oldvoicemails = 0;
		struct ast_smdi_mwi_message *mwi_msg;

		if (!ast_strlen_zero(context))
			snprintf(ext_context, sizeof(ext_context), "%s@%s", extension, context);
		else
			ast_copy_string(ext_context, extension, sizeof(ext_context));

		if (smdi_iface) {
			if (ast_app_has_voicemail(ext_context, NULL)) 
				ast_smdi_mwi_set(smdi_iface, extension);
			else
				ast_smdi_mwi_unset(smdi_iface, extension);

			if ((mwi_msg = ast_smdi_mwi_message_wait_station(smdi_iface, SMDI_MWI_WAIT_TIMEOUT, extension))) {
				ast_log(LOG_ERROR, "Error executing SMDI MWI change for %s\n", extension);
				if (!strncmp(mwi_msg->cause, "INV", 3))
					ast_log(LOG_ERROR, "Invalid MWI extension: %s\n", mwi_msg->fwd_st);
				else if (!strncmp(mwi_msg->cause, "BLK", 3))
					ast_log(LOG_WARNING, "MWI light was already on or off for %s\n", mwi_msg->fwd_st);
				ast_log(LOG_WARNING, "The switch reported '%s'\n", mwi_msg->cause);
				ASTOBJ_UNREF(mwi_msg, ast_smdi_mwi_message_destroy);
			} else {
				ast_debug(1, "Successfully executed SMDI MWI change for %s\n", extension);
			}
		}

		if (!ast_strlen_zero(externnotify)) {
			if (inboxcount(ext_context, &newvoicemails, &oldvoicemails)) {
				ast_log(LOG_ERROR, "Problem in calculating number of voicemail messages available for extension %s\n", extension);
			} else {
				snprintf(arguments, sizeof(arguments), "%s %s %s %d&", externnotify, context, extension, newvoicemails);
				ast_debug(1, "Executing %s\n", arguments);
				ast_safe_system(arguments);
			}
		}
	}

	struct leave_vm_options {
		unsigned int flags;
		signed char record_gain;
		char *exitcontext;
	};

	static int leave_voicemail(struct ast_channel *chan, char *ext, struct leave_vm_options *options)
	{
#ifdef IMAP_STORAGE
		int newmsgs, oldmsgs;
#endif
		char txtfile[PATH_MAX], tmptxtfile[PATH_MAX];
		struct vm_state *vms = NULL;
		char callerid[256];
		FILE *txt;
		char date[256];
		int txtdes;
		int res = 0;
		int msgnum;
		int duration = 0;
		int ausemacro = 0;
		int ousemacro = 0;
		int ouseexten = 0;
		int rtmsgid = 0;
		char tmpid[16];
		char tmpdur[16];
		char priority[16];
		char origtime[16];
		char dir[PATH_MAX], tmpdir[PATH_MAX];
		char fn[PATH_MAX];
		char prefile[PATH_MAX] = "";
		char tempfile[PATH_MAX] = "";
		char ext_context[256] = "";
		char fmt[80];
		char *context;
		char ecodes[17] = "#";
		char tmp[1024] = "", *tmpptr;
		struct ast_vm_user *vmu;
		struct ast_vm_user svm;
		const char *category = NULL, *code, *alldtmf = "0123456789ABCD*#";

		ast_copy_string(tmp, ext, sizeof(tmp));
		ext = tmp;
		if ((context = strchr(tmp, '@'))) {
			*context++ = '\0';
			tmpptr = strchr(context, '&');
		} else {
			tmpptr = strchr(ext, '&');
		}

		if (tmpptr)
			*tmpptr++ = '\0';

		category = pbx_builtin_getvar_helper(chan, "VM_CATEGORY");

		ast_debug(3, "Before find_user\n");
		if (!(vmu = find_user(&svm, context, ext))) {
			ast_log(LOG_WARNING, "No entry in voicemail config file for '%s'\n", ext);
			pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
			return res;
		}
		/* Setup pre-file if appropriate */
		if (strcmp(vmu->context, "default"))
			snprintf(ext_context, sizeof(ext_context), "%s@%s", ext, vmu->context);
		else
			ast_copy_string(ext_context, vmu->mailbox, sizeof(ext_context));
		if (ast_test_flag(options, OPT_BUSY_GREETING)) {
			snprintf(prefile, sizeof(prefile), "%s%s/%s/busy", VM_SPOOL_DIR, vmu->context, ext);
		} else if (ast_test_flag(options, OPT_UNAVAIL_GREETING)) {
			snprintf(prefile, sizeof(prefile), "%s%s/%s/unavail", VM_SPOOL_DIR, vmu->context, ext);
		}
		snprintf(tempfile, sizeof(tempfile), "%s%s/%s/temp", VM_SPOOL_DIR, vmu->context, ext);
		if ((res = create_dirpath(tmpdir, sizeof(tmpdir), vmu->context, ext, "tmp"))) {
			ast_log(LOG_WARNING, "Failed to make directory (%s)\n", tempfile);
			return -1;
		}
		RETRIEVE(tempfile, -1, vmu->mailbox, vmu->context);
		if (ast_fileexists(tempfile, NULL, NULL) > 0)
			ast_copy_string(prefile, tempfile, sizeof(prefile));
		DISPOSE(tempfile, -1);
		/* It's easier just to try to make it than to check for its existence */
		create_dirpath(dir, sizeof(dir), vmu->context, ext, "INBOX");

		/* Check current or macro-calling context for special extensions */
		if (ast_test_flag(vmu, VM_OPERATOR)) {
			if (!ast_strlen_zero(vmu->exit)) {
				if (ast_exists_extension(chan, vmu->exit, "o", 1, chan->cid.cid_num)) {
					strncat(ecodes, "0", sizeof(ecodes) - strlen(ecodes) - 1);
					ouseexten = 1;
				}
			} else if (ast_exists_extension(chan, chan->context, "o", 1, chan->cid.cid_num)) {
				strncat(ecodes, "0", sizeof(ecodes) - strlen(ecodes) - 1);
				ouseexten = 1;
			} else if (!ast_strlen_zero(chan->macrocontext) && ast_exists_extension(chan, chan->macrocontext, "o", 1, chan->cid.cid_num)) {
				strncat(ecodes, "0", sizeof(ecodes) - strlen(ecodes) - 1);
				ousemacro = 1;
			}
		}

		if (!ast_strlen_zero(vmu->exit)) {
			if (ast_exists_extension(chan, vmu->exit, "a", 1, chan->cid.cid_num))
				strncat(ecodes, "*", sizeof(ecodes) - strlen(ecodes) - 1);
		} else if (ast_exists_extension(chan, chan->context, "a", 1, chan->cid.cid_num))
			strncat(ecodes, "*", sizeof(ecodes) - strlen(ecodes) - 1);
		else if (!ast_strlen_zero(chan->macrocontext) && ast_exists_extension(chan, chan->macrocontext, "a", 1, chan->cid.cid_num)) {
			strncat(ecodes, "*", sizeof(ecodes) - strlen(ecodes) - 1);
			ausemacro = 1;
		}

		if (ast_test_flag(options, OPT_DTMFEXIT)) {
			for (code = alldtmf; *code; code++) {
				char e[2] = "";
				e[0] = *code;
				if (strchr(ecodes, e[0]) == NULL && ast_canmatch_extension(chan, chan->context, e, 1, chan->cid.cid_num))
					strncat(ecodes, e, sizeof(ecodes) - strlen(ecodes) - 1);
			}
		}

		/* Play the beginning intro if desired */
		if (!ast_strlen_zero(prefile)) {
			res = play_greeting(chan, vmu, prefile, ecodes);
			if (res == -2) {
				/* The file did not exist */
				ast_debug(1, "%s doesn't exist, doing what we can\n", prefile);
				res = invent_message(chan, vmu, ext, ast_test_flag(options, OPT_BUSY_GREETING), ecodes);
			}
			if (res < 0) {
				ast_debug(1, "Hang up during prefile playback\n");
				free_user(vmu);
				pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
				return -1;
			}
		}
		if (res == '#') {
			/* On a '#' we skip the instructions */
			ast_set_flag(options, OPT_SILENT);
			res = 0;
		}
		if (!res && !ast_test_flag(options, OPT_SILENT)) {
			res = ast_stream_and_wait(chan, INTRO, ecodes);
			if (res == '#') {
				ast_set_flag(options, OPT_SILENT);
				res = 0;
			}
		}
		if (res > 0)
			ast_stopstream(chan);
		/* Check for a '*' here in case the caller wants to escape from voicemail to something
		other than the operator -- an automated attendant or mailbox login for example */
		if (res == '*') {
			chan->exten[0] = 'a';
			chan->exten[1] = '\0';
			if (!ast_strlen_zero(vmu->exit)) {
				ast_copy_string(chan->context, vmu->exit, sizeof(chan->context));
			} else if (ausemacro && !ast_strlen_zero(chan->macrocontext)) {
				ast_copy_string(chan->context, chan->macrocontext, sizeof(chan->context));
			}
			chan->priority = 0;
			free_user(vmu);
			pbx_builtin_setvar_helper(chan, "VMSTATUS", "USEREXIT");
			return 0;
		}

		/* Check for a '0' here */
		if (ast_test_flag(vmu, VM_OPERATOR) && res == '0') {
		transfer:
			if (ouseexten || ousemacro) {
				chan->exten[0] = 'o';
				chan->exten[1] = '\0';
				if (!ast_strlen_zero(vmu->exit)) {
					ast_copy_string(chan->context, vmu->exit, sizeof(chan->context));
				} else if (ousemacro && !ast_strlen_zero(chan->macrocontext)) {
					ast_copy_string(chan->context, chan->macrocontext, sizeof(chan->context));
				}
				ast_play_and_wait(chan, "transfer");
				chan->priority = 0;
				free_user(vmu);
				pbx_builtin_setvar_helper(chan, "VMSTATUS", "USEREXIT");
			}
			return 0;
		}

		/* Allow all other digits to exit Voicemail and return to the dialplan */
		if (ast_test_flag(options, OPT_DTMFEXIT) && res > 0) {
			if (!ast_strlen_zero(options->exitcontext))
				ast_copy_string(chan->context, options->exitcontext, sizeof(chan->context));
			free_user(vmu);
			pbx_builtin_setvar_helper(chan, "VMSTATUS", "USEREXIT");
			return res;
		}

		if (res < 0) {
			free_user(vmu);
			pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
			return -1;
		}
		/* The meat of recording the message...  All the announcements and beeps have been played*/
		ast_copy_string(fmt, vmfmts, sizeof(fmt));
		if (!ast_strlen_zero(fmt)) {
			msgnum = 0;

#ifdef IMAP_STORAGE
			/* Is ext a mailbox? */
			/* must open stream for this user to get info! */
			res = inboxcount(ext_context, &newmsgs, &oldmsgs);
			if (res < 0) {
				ast_log(LOG_NOTICE, "Can not leave voicemail, unable to count messages\n");
				return -1;
			}
			if (!(vms = get_vm_state_by_mailbox(ext, 0))) {
			/* It is possible under certain circumstances that inboxcount did not
			* create a vm_state when it was needed. This is a catchall which will
			* rarely be used.
			*/
				if (!(vms = create_vm_state_from_user(vmu))) {
					ast_log(LOG_ERROR, "Couldn't allocate necessary space\n");
					return -1;
				}
			}
			vms->newmessages++;
			
			/* here is a big difference! We add one to it later */
			msgnum = newmsgs + oldmsgs;
			ast_debug(3, "Messagecount set to %d\n", msgnum);
			snprintf(fn, sizeof(fn), "%s/imap/msg%s%04d", VM_SPOOL_DIR, vmu->mailbox, msgnum);
			/* set variable for compatibility */
			pbx_builtin_setvar_helper(chan, "VM_MESSAGEFILE", "IMAP_STORAGE");

			/* Check if mailbox is full */
			check_quota(vms, imapfolder);
			if (vms->quota_limit && vms->quota_usage >= vms->quota_limit) {
				ast_debug(1, "*** QUOTA EXCEEDED!! %u >= %u\n", vms->quota_usage, vms->quota_limit);
				ast_play_and_wait(chan, "vm-mailboxfull");
				return -1;
			}
			
			/* Check if we have exceeded maxmsg */
			if (msgnum >= vmu->maxmsg) {
				ast_log(LOG_WARNING, "Unable to leave message since we will exceed the maximum number of messages allowed (%u > %u)\n", msgnum, vmu->maxmsg);
				ast_play_and_wait(chan, "vm-mailboxfull");
				return -1;
			}
#else
			if (count_messages(vmu, dir) >= vmu->maxmsg) {
				res = ast_streamfile(chan, "vm-mailboxfull", chan->language);
				if (!res)
					res = ast_waitstream(chan, "");
				ast_log(LOG_WARNING, "No more messages possible\n");
				pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
				goto leave_vm_out;
			}

#endif
			snprintf(tmptxtfile, sizeof(tmptxtfile), "%s/XXXXXX", tmpdir);
			txtdes = mkstemp(tmptxtfile);
			chmod(tmptxtfile, VOICEMAIL_FILE_MODE & ~my_umask);
			if (txtdes < 0) {
				res = ast_streamfile(chan, "vm-mailboxfull", chan->language);
				if (!res)
					res = ast_waitstream(chan, "");
				ast_log(LOG_ERROR, "Unable to create message file: %s\n", strerror(errno));
				pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
				goto leave_vm_out;
			}

			/* Now play the beep once we have the message number for our next message. */
			if (res >= 0) {
				/* Unless we're *really* silent, try to send the beep */
				res = ast_stream_and_wait(chan, "beep", "");
			}
					
			/* Store information in real-time storage */
			if (ast_check_realtime("voicemail_data")) {
				snprintf(priority, sizeof(priority), "%d", chan->priority);
				snprintf(origtime, sizeof(origtime), "%ld", (long)time(NULL));
				get_date(date, sizeof(date));
				rtmsgid = ast_store_realtime("voicemail_data", "origmailbox", ext, "context", chan->context, "macrocontext", chan->macrocontext, "exten", chan->exten, "priority", priority, "callerchan", chan->name, "callerid", ast_callerid_merge(callerid, sizeof(callerid), chan->cid.cid_name, chan->cid.cid_num, "Unknown"), "origdate", date, "origtime", origtime, "category", category ? category : "", NULL);
			}

			/* Store information */
			txt = fdopen(txtdes, "w+");
			if (txt) {
				get_date(date, sizeof(date));
				fprintf(txt, 
					";\n"
					"; Message Information file\n"
					";\n"
					"[message]\n"
					"origmailbox=%s\n"
					"context=%s\n"
					"macrocontext=%s\n"
					"exten=%s\n"
					"priority=%d\n"
					"callerchan=%s\n"
					"callerid=%s\n"
					"origdate=%s\n"
					"origtime=%ld\n"
					"category=%s\n",
					ext,
					chan->context,
					chan->macrocontext, 
					chan->exten,
					chan->priority,
					chan->name,
					ast_callerid_merge(callerid, sizeof(callerid), S_OR(chan->cid.cid_name, NULL), S_OR(chan->cid.cid_num, NULL), "Unknown"),
					date, (long)time(NULL),
					category ? category : ""); 
			} else
				ast_log(LOG_WARNING, "Error opening text file for output\n");
			res = play_record_review(chan, NULL, tmptxtfile, vmu->maxsecs, fmt, 1, vmu, &duration, NULL, options->record_gain, vms);

			if (txt) {
				if (duration < vmminsecs) {
					fclose(txt);
					ast_verb(3, "Recording was %d seconds long but needs to be at least %d - abandoning\n", duration, vmminsecs);
					ast_filedelete(tmptxtfile, NULL);
					unlink(tmptxtfile);
					if (ast_check_realtime("voicemail_data")) {
						snprintf(tmpid, sizeof(tmpid), "%d", rtmsgid);
						ast_destroy_realtime("voicemail_data", "id", tmpid, NULL);
					}
				} else {
					fprintf(txt, "duration=%d\n", duration);
					fclose(txt);
					if (vm_lock_path(dir)) {
						ast_log(LOG_ERROR, "Couldn't lock directory %s.  Voicemail will be lost.\n", dir);
						/* Delete files */
						ast_filedelete(tmptxtfile, NULL);
						unlink(tmptxtfile);
					} else if (ast_fileexists(tmptxtfile, NULL, NULL) <= 0) {
						ast_debug(1, "The recorded media file is gone, so we should remove the .txt file too!\n");
						unlink(tmptxtfile);
						ast_unlock_path(dir);
						if (ast_check_realtime("voicemail_data")) {
							snprintf(tmpid, sizeof(tmpid), "%d", rtmsgid);
							ast_destroy_realtime("voicemail_data", "id", tmpid, NULL);
						}
					} else {
#ifndef IMAP_STORAGE
						msgnum = last_message_index(vmu, dir) + 1;
#endif
						make_file(fn, sizeof(fn), dir, msgnum);

						/* assign a variable with the name of the voicemail file */ 
#ifndef IMAP_STORAGE
						pbx_builtin_setvar_helper(chan, "VM_MESSAGEFILE", fn);
#else
						pbx_builtin_setvar_helper(chan, "VM_MESSAGEFILE", "IMAP_STORAGE");
#endif

						snprintf(txtfile, sizeof(txtfile), "%s.txt", fn);
						ast_filerename(tmptxtfile, fn, NULL);
						rename(tmptxtfile, txtfile);

						/* Properly set permissions on voicemail text descriptor file.
						Unfortunately mkstemp() makes this file 0600 on most unix systems. */
						if (chmod(txtfile, VOICEMAIL_FILE_MODE) < 0)
							ast_log(LOG_ERROR, "Couldn't set permissions on voicemail text file %s: %s", txtfile, strerror(errno));

						ast_unlock_path(dir);
						if (ast_check_realtime("voicemail_data")) {
							snprintf(tmpid, sizeof(tmpid), "%d", rtmsgid);
							snprintf(tmpdur, sizeof(tmpdur), "%d", duration);
							ast_update_realtime("voicemail_data", "id", tmpid, "filename", fn, "duration", tmpdur, NULL);
						}
						/* We must store the file first, before copying the message, because
						* ODBC storage does the entire copy with SQL.
						*/
						if (ast_fileexists(fn, NULL, NULL) > 0) {
							STORE(dir, vmu->mailbox, vmu->context, msgnum, chan, vmu, fmt, duration, vms);
						}

						/* Are there to be more recipients of this message? */
						while (tmpptr) {
							struct ast_vm_user recipu, *recip;
							char *exten, *context;
						
							exten = strsep(&tmpptr, "&");
							context = strchr(exten, '@');
							if (context) {
								*context = '\0';
								context++;
							}
							if ((recip = find_user(&recipu, context, exten))) {
								copy_message(chan, vmu, 0, msgnum, duration, recip, fmt, dir);
								free_user(recip);
							}
						}
						/* Notification and disposal needs to happen after the copy, though. */
						if (ast_fileexists(fn, NULL, NULL)) {
#ifdef IMAP_STORAGE
							notify_new_message(chan, vmu, vms, msgnum, duration, fmt, S_OR(chan->cid.cid_num, NULL), S_OR(chan->cid.cid_name, NULL));
#else
							notify_new_message(chan, vmu, NULL, msgnum, duration, fmt, S_OR(chan->cid.cid_num, NULL), S_OR(chan->cid.cid_name, NULL));
#endif
							DISPOSE(dir, msgnum);
						}
					}
				}
			}
			if (res == '0') {
				goto transfer;
			} else if (res > 0)
				res = 0;

			if (duration < vmminsecs)
				/* XXX We should really give a prompt too short/option start again, with leave_vm_out called only after a timeout XXX */
				pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
			else
				pbx_builtin_setvar_helper(chan, "VMSTATUS", "SUCCESS");
		} else
			ast_log(LOG_WARNING, "No format for saving voicemail?\n");
	leave_vm_out:
		free_user(vmu);
		
		return res;
	}

#ifndef IMAP_STORAGE
	static int resequence_mailbox(struct ast_vm_user *vmu, char *dir)
	{
		/* we know max messages, so stop process when number is hit */

		int x, dest;
		char sfn[PATH_MAX];
		char dfn[PATH_MAX];

		if (vm_lock_path(dir))
			return ERROR_LOCK_PATH;

		for (x = 0, dest = 0; x < vmu->maxmsg; x++) {
			make_file(sfn, sizeof(sfn), dir, x);
			if (EXISTS(dir, x, sfn, NULL)) {
				
				if (x != dest) {
					make_file(dfn, sizeof(dfn), dir, dest);
					RENAME(dir, x, vmu->mailbox, vmu->context, dir, dest, sfn, dfn);
				}
				
				dest++;
			}
		}
		ast_unlock_path(dir);

		return 0;
	}
#endif

	static int say_and_wait(struct ast_channel *chan, int num, const char *language)
	{
		int d;
		d = ast_say_number(chan, num, AST_DIGIT_ANY, language, NULL);
		return d;
	}

	static int save_to_folder(struct ast_vm_user *vmu, struct vm_state *vms, int msg, int box)
	{
#ifdef IMAP_STORAGE
		/* we must use mbox(x) folder names, and copy the message there */
		/* simple. huh? */
		char sequence[10];
		char mailbox[256];
		/* get the real IMAP message number for this message */
		snprintf(sequence, sizeof(sequence), "%ld", vms->msgArray[msg]);
		
		ast_debug(3, "Copying sequence %s to mailbox %s\n", sequence, mbox(box));
		if (box == OLD_FOLDER) {
			mail_setflag(vms->mailstream, sequence, "\\Seen");
		} else if (box == NEW_FOLDER) {
			mail_clearflag(vms->mailstream, sequence, "\\Seen");
		}
		if (!strcasecmp(mbox(NEW_FOLDER), vms->curbox) && (box == NEW_FOLDER || box == OLD_FOLDER))
			return 0;
		/* Create the folder if it don't exist */
		imap_mailbox_name(mailbox, sizeof(mailbox), vms, box, 1); /* Get the full mailbox name */
		ast_debug(5, "Checking if folder exists: %s\n",mailbox);
		if (mail_create(vms->mailstream, mailbox) == NIL) 
			ast_debug(5, "Folder exists.\n");
		else
			ast_log(LOG_NOTICE, "Folder %s created!\n",mbox(box));
		return !mail_copy(vms->mailstream, sequence, (char *)mbox(box));
#else
		char *dir = vms->curdir;
		char *username = vms->username;
		char *context = vmu->context;
		char sfn[PATH_MAX];
		char dfn[PATH_MAX];
		char ddir[PATH_MAX];
		const char *dbox = mbox(box);
		int x, i;
		create_dirpath(ddir, sizeof(ddir), context, username, dbox);

		if (vm_lock_path(ddir))
			return ERROR_LOCK_PATH;

		x = last_message_index(vmu, ddir) + 1;

		if (box == 10 && x >= vmu->maxdeletedmsg) { /* "Deleted" folder*/
			x--;
			for (i = 1; i <= x; i++) {
				/* Push files down a "slot".  The oldest file (msg0000) will be deleted. */
				make_file(sfn, sizeof(sfn), ddir, i);
				make_file(dfn, sizeof(dfn), ddir, i - 1);
				if (EXISTS(ddir, i, sfn, NULL)) {
					RENAME(ddir, i, vmu->mailbox, vmu->context, ddir, i - 1, sfn, dfn);
				} else
					break;
			}
		} else {
			if (x >= vmu->maxmsg) {
				ast_unlock_path(ddir);
				return ERROR_MAILBOX_FULL;
			}
		}
		make_file(sfn, sizeof(sfn), dir, msg);
		make_file(dfn, sizeof(dfn), ddir, x);
		if (strcmp(sfn, dfn)) {
			COPY(dir, msg, ddir, x, username, context, sfn, dfn);
		}
		ast_unlock_path(ddir);
#endif
		return 0;
	}

	static int adsi_logo(unsigned char *buf)
	{
		int bytes = 0;
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_CENT, 0, "Comedian Mail", "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_CENT, 0, "(C)2002-2006 Digium, Inc.", "");
		return bytes;
	}

	static int adsi_load_vmail(struct ast_channel *chan, int *useadsi)
	{
		unsigned char buf[256];
		int bytes = 0;
		int x;
		char num[5];

		*useadsi = 0;
		bytes += ast_adsi_data_mode(buf + bytes);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);

		bytes = 0;
		bytes += adsi_logo(buf);
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Downloading Scripts", "");
#ifdef DISPLAY
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   .", "");
#endif
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_data_mode(buf + bytes);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);

		if (ast_adsi_begin_download(chan, addesc, adsifdn, adsisec, adsiver)) {
			bytes = 0;
			bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Load Cancelled.", "");
			bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "ADSI Unavailable", "");
			bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
			bytes += ast_adsi_voice_mode(buf + bytes, 0);
			ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
			return 0;
		}

#ifdef DISPLAY
		/* Add a dot */
		bytes = 0;
		bytes += ast_adsi_logo(buf);
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Downloading Scripts", "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   ..", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif
		bytes = 0;
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 0, "Listen", "Listen", "1", 1);
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 1, "Folder", "Folder", "2", 1);
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 2, "Advanced", "Advnced", "3", 1);
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 3, "Options", "Options", "0", 1);
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 4, "Help", "Help", "*", 1);
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 5, "Exit", "Exit", "#", 1);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

#ifdef DISPLAY
		/* Add another dot */
		bytes = 0;
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   ...", "");
		bytes += ast_adsi_voice_mode(buf + bytes, 0);

		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif

		bytes = 0;
		/* These buttons we load but don't use yet */
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 6, "Previous", "Prev", "4", 1);
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 8, "Repeat", "Repeat", "5", 1);
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 7, "Delete", "Delete", "7", 1);
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 9, "Next", "Next", "6", 1);
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 10, "Save", "Save", "9", 1);
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 11, "Undelete", "Restore", "7", 1);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

#ifdef DISPLAY
		/* Add another dot */
		bytes = 0;
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   ....", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif

		bytes = 0;
		for (x = 0; x < 5; x++) {
			snprintf(num, sizeof(num), "%d", x);
			bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 12 + x, mbox(x), mbox(x), num, 1);
		}
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 12 + 5, "Cancel", "Cancel", "#", 1);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

#ifdef DISPLAY
		/* Add another dot */
		bytes = 0;
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, "   .....", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
#endif

		if (ast_adsi_end_download(chan)) {
			bytes = 0;
			bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Download Unsuccessful.", "");
			bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "ADSI Unavailable", "");
			bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
			bytes += ast_adsi_voice_mode(buf + bytes, 0);
			ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
			return 0;
		}
		bytes = 0;
		bytes += ast_adsi_download_disconnect(buf + bytes);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DOWNLOAD);

		ast_debug(1, "Done downloading scripts...\n");

#ifdef DISPLAY
		/* Add last dot */
		bytes = 0;
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "   ......", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
#endif
		ast_debug(1, "Restarting session...\n");

		bytes = 0;
		/* Load the session now */
		if (ast_adsi_load_session(chan, adsifdn, adsiver, 1) == 1) {
			*useadsi = 1;
			bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Scripts Loaded!", "");
		} else
			bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Load Failed!", "");

		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
		return 0;
	}

	static void adsi_begin(struct ast_channel *chan, int *useadsi)
	{
		int x;
		if (!ast_adsi_available(chan))
			return;
		x = ast_adsi_load_session(chan, adsifdn, adsiver, 1);
		if (x < 0)
			return;
		if (!x) {
			if (adsi_load_vmail(chan, useadsi)) {
				ast_log(LOG_WARNING, "Unable to upload voicemail scripts\n");
				return;
			}
		} else
			*useadsi = 1;
	}

	static void adsi_login(struct ast_channel *chan)
	{
		unsigned char buf[256];
		int bytes = 0;
		unsigned char keys[8];
		int x;
		if (!ast_adsi_available(chan))
			return;

		for (x = 0; x < 8; x++)
			keys[x] = 0;
		/* Set one key for next */
		keys[3] = ADSI_KEY_APPS + 3;

		bytes += adsi_logo(buf + bytes);
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, " ", "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, " ", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_input_format(buf + bytes, 1, ADSI_DIR_FROM_LEFT, 0, "Mailbox: ******", "");
		bytes += ast_adsi_input_control(buf + bytes, ADSI_COMM_PAGE, 4, 1, 1, ADSI_JUST_LEFT);
		bytes += ast_adsi_load_soft_key(buf + bytes, ADSI_KEY_APPS + 3, "Enter", "Enter", "#", 1);
		bytes += ast_adsi_set_keys(buf + bytes, keys);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	static void adsi_password(struct ast_channel *chan)
	{
		unsigned char buf[256];
		int bytes = 0;
		unsigned char keys[8];
		int x;
		if (!ast_adsi_available(chan))
			return;

		for (x = 0; x < 8; x++)
			keys[x] = 0;
		/* Set one key for next */
		keys[3] = ADSI_KEY_APPS + 3;

		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_input_format(buf + bytes, 1, ADSI_DIR_FROM_LEFT, 0, "Password: ******", "");
		bytes += ast_adsi_input_control(buf + bytes, ADSI_COMM_PAGE, 4, 0, 1, ADSI_JUST_LEFT);
		bytes += ast_adsi_set_keys(buf + bytes, keys);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	static void adsi_folders(struct ast_channel *chan, int start, char *label)
	{
		unsigned char buf[256];
		int bytes = 0;
		unsigned char keys[8];
		int x, y;

		if (!ast_adsi_available(chan))
			return;

		for (x = 0; x < 5; x++) {
			y = ADSI_KEY_APPS + 12 + start + x;
			if (y > ADSI_KEY_APPS + 12 + 4)
				y = 0;
			keys[x] = ADSI_KEY_SKT | y;
		}
		keys[5] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 17);
		keys[6] = 0;
		keys[7] = 0;

		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_CENT, 0, label, "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_CENT, 0, " ", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_set_keys(buf + bytes, keys);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);

		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	static void adsi_message(struct ast_channel *chan, struct vm_state *vms)
	{
		int bytes = 0;
		unsigned char buf[256]; 
		char buf1[256], buf2[256];
		char fn2[PATH_MAX];

		char cid[256] = "";
		char *val;
		char *name, *num;
		char datetime[21] = "";
		FILE *f;

		unsigned char keys[8];

		int x;

		if (!ast_adsi_available(chan))
			return;

		/* Retrieve important info */
		snprintf(fn2, sizeof(fn2), "%s.txt", vms->fn);
		f = fopen(fn2, "r");
		if (f) {
			while (!feof(f)) {	
				fgets((char *)buf, sizeof(buf), f);
				if (!feof(f)) {
					char *stringp = NULL;
					stringp = (char *)buf;
					strsep(&stringp, "=");
					val = strsep(&stringp, "=");
					if (!ast_strlen_zero(val)) {
						if (!strcmp((char *)buf, "callerid"))
							ast_copy_string(cid, val, sizeof(cid));
						if (!strcmp((char *)buf, "origdate"))
							ast_copy_string(datetime, val, sizeof(datetime));
					}
				}
			}
			fclose(f);
		}
		/* New meaning for keys */
		for (x = 0; x < 5; x++)
			keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 6 + x);
		keys[6] = 0x0;
		keys[7] = 0x0;

		if (!vms->curmsg) {
			/* No prev key, provide "Folder" instead */
			keys[0] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
		}
		if (vms->curmsg >= vms->lastmsg) {
			/* If last message ... */
			if (vms->curmsg) {
				/* but not only message, provide "Folder" instead */
				keys[3] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
				bytes += ast_adsi_voice_mode(buf + bytes, 0);

			} else {
				/* Otherwise if only message, leave blank */
				keys[3] = 1;
			}
		}

		if (!ast_strlen_zero(cid)) {
			ast_callerid_parse(cid, &name, &num);
			if (!name)
				name = num;
		} else
			name = "Unknown Caller";

		/* If deleted, show "undeleted" */

		if (vms->deleted[vms->curmsg])
			keys[1] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 11);

		/* Except "Exit" */
		keys[5] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 5);
		snprintf(buf1, sizeof(buf1), "%s%s", vms->curbox,
			strcasecmp(vms->curbox, "INBOX") ? " Messages" : "");
		snprintf(buf2, sizeof(buf2), "Message %d of %d", vms->curmsg + 1, vms->lastmsg + 1);

		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_LEFT, 0, buf1, "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_LEFT, 0, buf2, "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_LEFT, 0, name, "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_LEFT, 0, datetime, "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_set_keys(buf + bytes, keys);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);

		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	static void adsi_delete(struct ast_channel *chan, struct vm_state *vms)
	{
		int bytes = 0;
		unsigned char buf[256];
		unsigned char keys[8];

		int x;

		if (!ast_adsi_available(chan))
			return;

		/* New meaning for keys */
		for (x = 0; x < 5; x++)
			keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 6 + x);

		keys[6] = 0x0;
		keys[7] = 0x0;

		if (!vms->curmsg) {
			/* No prev key, provide "Folder" instead */
			keys[0] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
		}
		if (vms->curmsg >= vms->lastmsg) {
			/* If last message ... */
			if (vms->curmsg) {
				/* but not only message, provide "Folder" instead */
				keys[3] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 1);
			} else {
				/* Otherwise if only message, leave blank */
				keys[3] = 1;
			}
		}

		/* If deleted, show "undeleted" */
		if (vms->deleted[vms->curmsg]) 
			keys[1] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 11);

		/* Except "Exit" */
		keys[5] = ADSI_KEY_SKT | (ADSI_KEY_APPS + 5);
		bytes += ast_adsi_set_keys(buf + bytes, keys);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);

		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	static void adsi_status(struct ast_channel *chan, struct vm_state *vms)
	{
		unsigned char buf[256] = "";
		char buf1[256] = "", buf2[256] = "";
		int bytes = 0;
		unsigned char keys[8];
		int x;

		char *newm = (vms->newmessages == 1) ? "message" : "messages";
		char *oldm = (vms->oldmessages == 1) ? "message" : "messages";
		if (!ast_adsi_available(chan))
			return;
		if (vms->newmessages) {
			snprintf(buf1, sizeof(buf1), "You have %d new", vms->newmessages);
			if (vms->oldmessages) {
				strncat(buf1, " and", sizeof(buf1) - strlen(buf1) - 1);
				snprintf(buf2, sizeof(buf2), "%d old %s.", vms->oldmessages, oldm);
			} else {
				snprintf(buf2, sizeof(buf2), "%s.", newm);
			}
		} else if (vms->oldmessages) {
			snprintf(buf1, sizeof(buf1), "You have %d old", vms->oldmessages);
			snprintf(buf2, sizeof(buf2), "%s.", oldm);
		} else {
			strcpy(buf1, "You have no messages.");
			buf2[0] = ' ';
			buf2[1] = '\0';
		}
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_LEFT, 0, buf1, "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_LEFT, 0, buf2, "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);

		for (x = 0; x < 6; x++)
			keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + x);
		keys[6] = 0;
		keys[7] = 0;

		/* Don't let them listen if there are none */
		if (vms->lastmsg < 0)
			keys[0] = 1;
		bytes += ast_adsi_set_keys(buf + bytes, keys);

		bytes += ast_adsi_voice_mode(buf + bytes, 0);

		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	static void adsi_status2(struct ast_channel *chan, struct vm_state *vms)
	{
		unsigned char buf[256] = "";
		char buf1[256] = "", buf2[256] = "";
		int bytes = 0;
		unsigned char keys[8];
		int x;

		char *mess = (vms->lastmsg == 0) ? "message" : "messages";

		if (!ast_adsi_available(chan))
			return;

		/* Original command keys */
		for (x = 0; x < 6; x++)
			keys[x] = ADSI_KEY_SKT | (ADSI_KEY_APPS + x);

		keys[6] = 0;
		keys[7] = 0;

		if ((vms->lastmsg + 1) < 1)
			keys[0] = 0;

		snprintf(buf1, sizeof(buf1), "%s%s has", vms->curbox,
			strcasecmp(vms->curbox, "INBOX") ? " folder" : "");

		if (vms->lastmsg + 1)
			snprintf(buf2, sizeof(buf2), "%d %s.", vms->lastmsg + 1, mess);
		else
			strcpy(buf2, "no messages.");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 1, ADSI_JUST_LEFT, 0, buf1, "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 2, ADSI_JUST_LEFT, 0, buf2, "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_LEFT, 0, "", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_set_keys(buf + bytes, keys);

		bytes += ast_adsi_voice_mode(buf + bytes, 0);

		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
		
	}

	/*
	static void adsi_clear(struct ast_channel *chan)
	{
		char buf[256];
		int bytes = 0;
		if (!ast_adsi_available(chan))
			return;
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);

		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}
	*/

	static void adsi_goodbye(struct ast_channel *chan)
	{
		unsigned char buf[256];
		int bytes = 0;

		if (!ast_adsi_available(chan))
			return;
		bytes += adsi_logo(buf + bytes);
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_LEFT, 0, " ", "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Goodbye", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);

		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	/*!\brief get_folder: Folder menu
	*	Plays "press 1 for INBOX messages" etc.
	*	Should possibly be internationalized
	*/
	static int get_folder(struct ast_channel *chan, int start)
	{
		int x;
		int d;
		char fn[PATH_MAX];
		d = ast_play_and_wait(chan, "vm-press");	/* "Press" */
		if (d)
			return d;
		for (x = start; x < 5; x++) {	/* For all folders */
			if ((d = ast_say_number(chan, x, AST_DIGIT_ANY, chan->language, NULL)))
				return d;
			d = ast_play_and_wait(chan, "vm-for");	/* "for" */
			if (d)
				return d;
			snprintf(fn, sizeof(fn), "vm-%s", mbox(x));	/* Folder name */
			d = vm_play_folder_name(chan, fn);
			if (d)
				return d;
			d = ast_waitfordigit(chan, 500);
			if (d)
				return d;
		}
		d = ast_play_and_wait(chan, "vm-tocancel"); /* "or pound to cancel" */
		if (d)
			return d;
		d = ast_waitfordigit(chan, 4000);
		return d;
	}

	static int get_folder2(struct ast_channel *chan, char *fn, int start)
	{
		int res = 0;
		res = ast_play_and_wait(chan, fn);	/* Folder name */
		while (((res < '0') || (res > '9')) &&
				(res != '#') && (res >= 0)) {
			res = get_folder(chan, 0);
		}
		return res;
	}

	static int vm_forwardoptions(struct ast_channel *chan, struct ast_vm_user *vmu, char *curdir, int curmsg, char *vmfmts,
				char *context, signed char record_gain, long *duration, struct vm_state *vms)
	{
		int cmd = 0;
		int retries = 0, prepend_duration = 0, already_recorded = 0;
		signed char zero_gain = 0;
		struct ast_flags config_flags = { CONFIG_FLAG_NOCACHE };
		struct ast_config *msg_cfg;
		const char *duration_str;
		char msgfile[PATH_MAX], backup[PATH_MAX];
		char textfile[PATH_MAX];

		/* Must always populate duration correctly */
		make_file(msgfile, sizeof(msgfile), curdir, curmsg);
		strcpy(textfile, msgfile);
		strcpy(backup, msgfile);
		strncat(textfile, ".txt", sizeof(textfile) - strlen(textfile) - 1);
		strncat(backup, "-bak", sizeof(backup) - strlen(backup) - 1);

		if ((msg_cfg = ast_config_load(textfile, config_flags)) && (duration_str = ast_variable_retrieve(msg_cfg, "message", "duration"))) {
			*duration = atoi(duration_str);
		} else {
			*duration = 0;
		}

		while ((cmd >= 0) && (cmd != 't') && (cmd != '*')) {
			if (cmd)
				retries = 0;
			switch (cmd) {
			case '1': 
				/* prepend a message to the current message, update the metadata and return */
				prepend_duration = 0;

				/* if we can't read the message metadata, stop now */
				if (!msg_cfg) {
					cmd = 0;
					break;
				}

				/* Back up the original file, so we can retry the prepend */
				if (already_recorded)
					ast_filecopy(backup, msgfile, NULL);
				else
					ast_filecopy(msgfile, backup, NULL);
				already_recorded = 1;

				if (record_gain)
					ast_channel_setoption(chan, AST_OPTION_RXGAIN, &record_gain, sizeof(record_gain), 0);

				cmd = ast_play_and_prepend(chan, NULL, msgfile, 0, vmfmts, &prepend_duration, 1, silencethreshold, maxsilence);
				if (record_gain)
					ast_channel_setoption(chan, AST_OPTION_RXGAIN, &zero_gain, sizeof(zero_gain), 0);

				if (prepend_duration) {
					struct ast_category *msg_cat;
					/* need enough space for a maximum-length message duration */
					char duration_str[12];

					prepend_duration += *duration;
					msg_cat = ast_category_get(msg_cfg, "message");
					snprintf(duration_str, sizeof(duration_str), "%d", prepend_duration);
					if (!ast_variable_update(msg_cat, "duration", duration_str, NULL, 0)) {
						config_text_file_save(textfile, msg_cfg, "app_voicemail");
						STORE(curdir, vmu->mailbox, context, curmsg, chan, vmu, vmfmts, prepend_duration, vms);
					}
				}

				break;
			case '2': 
				cmd = 't';
				break;
			case '*':
				cmd = '*';
				break;
			default: 
				cmd = ast_play_and_wait(chan, "vm-forwardoptions");
					/* "Press 1 to prepend a message or 2 to forward the message without prepending" */
				if (!cmd)
					cmd = ast_play_and_wait(chan, "vm-starmain");
					/* "press star to return to the main menu" */
				if (!cmd)
					cmd = ast_waitfordigit(chan, 6000);
				if (!cmd)
					retries++;
				if (retries > 3)
					cmd = 't';
			}
		}

		if (msg_cfg)
			ast_config_destroy(msg_cfg);
		if (already_recorded)
			ast_filedelete(backup, NULL);
		if (prepend_duration)
			*duration = prepend_duration;

		if (cmd == 't' || cmd == 'S')
			cmd = 0;
		return cmd;
	}

	static void queue_mwi_event(const char *mbox, int new, int old)
	{
		struct ast_event *event;
		char *mailbox, *context;

		/* Strip off @default */
		context = mailbox = ast_strdupa(mbox);
		strsep(&context, "@");
		if (ast_strlen_zero(context))
			context = "default";

		if (!(event = ast_event_new(AST_EVENT_MWI,
				AST_EVENT_IE_MAILBOX, AST_EVENT_IE_PLTYPE_STR, mailbox,
				AST_EVENT_IE_CONTEXT, AST_EVENT_IE_PLTYPE_STR, context,
				AST_EVENT_IE_NEWMSGS, AST_EVENT_IE_PLTYPE_UINT, new,
				AST_EVENT_IE_OLDMSGS, AST_EVENT_IE_PLTYPE_UINT, old,
				AST_EVENT_IE_END))) {
			return;
		}

		ast_event_queue_and_cache(event,
			AST_EVENT_IE_MAILBOX, AST_EVENT_IE_PLTYPE_STR,
			AST_EVENT_IE_CONTEXT, AST_EVENT_IE_PLTYPE_STR,
			AST_EVENT_IE_END);
	}

	static int notify_new_message(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, int msgnum, long duration, char *fmt, char *cidnum, char *cidname)
	{
		char todir[PATH_MAX], fn[PATH_MAX], ext_context[PATH_MAX], *stringp;
		int newmsgs = 0, oldmsgs = 0;
		const char *category = pbx_builtin_getvar_helper(chan, "VM_CATEGORY");
		char *myserveremail = serveremail;

		make_dir(todir, sizeof(todir), vmu->context, vmu->mailbox, "INBOX");
		make_file(fn, sizeof(fn), todir, msgnum);
		snprintf(ext_context, sizeof(ext_context), "%s@%s", vmu->mailbox, vmu->context);

		if (!ast_strlen_zero(vmu->attachfmt)) {
			if (strstr(fmt, vmu->attachfmt))
				fmt = vmu->attachfmt;
			else
				ast_log(LOG_WARNING, "Attachment format '%s' is not one of the recorded formats '%s'.  Falling back to default format for '%s@%s'.\n", vmu->attachfmt, fmt, vmu->mailbox, vmu->context);
		}

		/* Attach only the first format */
		fmt = ast_strdupa(fmt);
		stringp = fmt;
		strsep(&stringp, "|");

		if (!ast_strlen_zero(vmu->serveremail))
			myserveremail = vmu->serveremail;

		if (!ast_strlen_zero(vmu->email)) {
			int attach_user_voicemail = ast_test_flag(vmu, VM_ATTACH);
			if (!attach_user_voicemail)
				attach_user_voicemail = ast_test_flag((&globalflags), VM_ATTACH);

			if (attach_user_voicemail)
				RETRIEVE(todir, msgnum, vmu->mailbox, vmu->context);

			/* XXX possible imap issue, should category be NULL XXX */
			sendmail(myserveremail, vmu, msgnum, vmu->context, vmu->mailbox, cidnum, cidname, fn, fmt, duration, attach_user_voicemail, chan, category);

			if (attach_user_voicemail)
				DISPOSE(todir, msgnum);
		}

		if (!ast_strlen_zero(vmu->pager))
			sendpage(myserveremail, vmu->pager, msgnum, vmu->context, vmu->mailbox, cidnum, cidname, duration, vmu, category);

		if (ast_test_flag(vmu, VM_DELETE))
			DELETE(todir, msgnum, fn, vmu);

		/* Leave voicemail for someone */
		if (ast_app_has_voicemail(ext_context, NULL)) 
			ast_app_inboxcount(ext_context, &newmsgs, &oldmsgs);

		queue_mwi_event(ext_context, newmsgs, oldmsgs);

		manager_event(EVENT_FLAG_CALL, "MessageWaiting", "Mailbox: %s@%s\r\nWaiting: %d\r\nNew: %d\r\nOld: %d\r\n", vmu->mailbox, vmu->context, ast_app_has_voicemail(ext_context, NULL), newmsgs, oldmsgs);
		run_externnotify(vmu->context, vmu->mailbox);

		return 0;
	}

	static int forward_message(struct ast_channel *chan, char *context, struct vm_state *vms, struct ast_vm_user *sender, char *fmt, int flag, signed char record_gain)
	{
#ifdef IMAP_STORAGE
		int todircount = 0;
		struct vm_state *dstvms;
#endif
		char username[70] = "";
		char fn[PATH_MAX]; /* for playback of name greeting */
		char ecodes[16] = "#";
		int res = 0, cmd = 0;
		struct ast_vm_user *receiver = NULL, *vmtmp;
		AST_LIST_HEAD_NOLOCK_STATIC(extensions, ast_vm_user);
		char *stringp;
		const char *s;
		int saved_messages = 0, found = 0;
		int valid_extensions = 0;
		char *dir;
		int curmsg;

		if (vms == NULL) return -1;
		dir = vms->curdir;
		curmsg = vms->curmsg;
		
		while (!res && !valid_extensions) {
			int use_directory = 0;
			if (ast_test_flag((&globalflags), VM_DIRECFORWARD)) {
				int done = 0;
				int retries = 0;
				cmd = 0;
				while ((cmd >= 0) && !done) {
					if (cmd)
						retries = 0;
					switch (cmd) {
					case '1': 
						use_directory = 0;
						done = 1;
						break;
					case '2': 
						use_directory = 1;
						done = 1;
						break;
					case '*': 
						cmd = 't';
						done = 1;
						break;
					default: 
						/* Press 1 to enter an extension press 2 to use the directory */
						cmd = ast_play_and_wait(chan, "vm-forward");
						if (!cmd)
							cmd = ast_waitfordigit(chan, 3000);
						if (!cmd)
							retries++;
						if (retries > 3) {
							cmd = 't';
							done = 1;
						}
						
					}
				}
				if (cmd < 0 || cmd == 't')
					break;
			}
			
			if (use_directory) {
				/* use app_directory */
				
				char old_context[sizeof(chan->context)];
				char old_exten[sizeof(chan->exten)];
				int old_priority;
				struct ast_app* app;

				
				app = pbx_findapp("Directory");
				if (app) {
					char vmcontext[256];
					/* make backup copies */
					memcpy(old_context, chan->context, sizeof(chan->context));
					memcpy(old_exten, chan->exten, sizeof(chan->exten));
					old_priority = chan->priority;
					
					/* call the the Directory, changes the channel */
					snprintf(vmcontext, sizeof(vmcontext), "%s||v", context ? context : "default");
					res = pbx_exec(chan, app, vmcontext);
					
					ast_copy_string(username, chan->exten, sizeof(username));
					
					/* restore the old context, exten, and priority */
					memcpy(chan->context, old_context, sizeof(chan->context));
					memcpy(chan->exten, old_exten, sizeof(chan->exten));
					chan->priority = old_priority;
					
				} else {
					ast_log(LOG_WARNING, "Could not find the Directory application, disabling directory_forward\n");
					ast_clear_flag((&globalflags), VM_DIRECFORWARD);
				}
			} else {
				/* Ask for an extension */
				res = ast_streamfile(chan, "vm-extension", chan->language);	/* "extension" */
				if (res)
					break;
				if ((res = ast_readstring(chan, username, sizeof(username) - 1, 2000, 10000, "#") < 0))
					break;
			}
			
			/* start all over if no username */
			if (ast_strlen_zero(username))
				continue;
			stringp = username;
			s = strsep(&stringp, "*");
			/* start optimistic */
			valid_extensions = 1;
			while (s) {
				/* Don't forward to ourselves but allow leaving a message for ourselves (flag == 1).  find_user is going to malloc since we have a NULL as first argument */
				if ((flag == 1 || strcmp(s, sender->mailbox)) && (receiver = find_user(NULL, context, s))) {
					AST_LIST_INSERT_HEAD(&extensions, receiver, list);
					found++;
				} else {
					valid_extensions = 0;
					break;
				}

				/* play name if available, else play extension number */
				snprintf(fn, sizeof(fn), "%s%s/%s/greet", VM_SPOOL_DIR, receiver->context, s);
				RETRIEVE(fn, -1, s, receiver->context);
				if (ast_fileexists(fn, NULL, NULL) > 0) {
					res = ast_stream_and_wait(chan, fn, ecodes);
					if (res) {
						DISPOSE(fn, -1);
						return res;
					}
				} else {
					/* Dispose just in case */
					DISPOSE(fn, -1);
					res = ast_say_digit_str(chan, s, ecodes, chan->language);
				}

				s = strsep(&stringp, "*");
			}
			/* break from the loop of reading the extensions */
			if (valid_extensions)
				break;
			/* "I am sorry, that's not a valid extension.  Please try again." */
			res = ast_play_and_wait(chan, "pbx-invalid");
		}
		/* check if we're clear to proceed */
		if (AST_LIST_EMPTY(&extensions) || !valid_extensions)
			return res;
		if (flag==1) {
			struct leave_vm_options leave_options;
			char mailbox[AST_MAX_EXTENSION * 2 + 2];
			/* Make sure that context doesn't get set as a literal "(null)" (or else find_user won't find it) */
			if (context)
				snprintf(mailbox, sizeof(mailbox), "%s@%s", username, context);
			else
				ast_copy_string(mailbox, username, sizeof(mailbox));

			/* Send VoiceMail */
			memset(&leave_options, 0, sizeof(leave_options));
			leave_options.record_gain = record_gain;
			cmd = leave_voicemail(chan, mailbox, &leave_options);
		} else {
			/* Forward VoiceMail */
			long duration = 0;
			char origmsgfile[PATH_MAX], msgfile[PATH_MAX];
			struct vm_state vmstmp;
			memcpy(&vmstmp, vms, sizeof(vmstmp));

			make_file(origmsgfile, sizeof(origmsgfile), dir, curmsg);
			create_dirpath(vmstmp.curdir, sizeof(vmstmp.curdir), sender->context, vmstmp.username, "tmp");
			make_file(msgfile, sizeof(msgfile), vmstmp.curdir, curmsg);
			
			RETRIEVE(dir, curmsg, sender->mailbox, sender->context);
			
			/* Alter a surrogate file, only */
			copy_plain_file(origmsgfile, msgfile);

			cmd = vm_forwardoptions(chan, sender, vmstmp.curdir, curmsg, vmfmts, S_OR(context, "default"), record_gain, &duration, &vmstmp);
			if (!cmd) {
				AST_LIST_TRAVERSE_SAFE_BEGIN(&extensions, vmtmp, list) {
#ifdef IMAP_STORAGE
					char *myserveremail = serveremail;
					int attach_user_voicemail;
					/* get destination mailbox */
					dstvms = get_vm_state_by_mailbox(vmtmp->mailbox, 0);
					if (!dstvms) {
						dstvms = create_vm_state_from_user(vmtmp);
					}
					if (dstvms) {
						init_mailstream(dstvms, 0);
						if (!dstvms->mailstream) {
							ast_log(LOG_ERROR, "IMAP mailstream for %s is NULL\n", vmtmp->mailbox);
						} else {
							STORE(vmstmp.curdir, vmtmp->mailbox, vmtmp->context, dstvms->curmsg, chan, vmtmp, fmt, duration, dstvms);
							run_externnotify(vmtmp->context, vmtmp->mailbox); 
						}
					} else {
						ast_log(LOG_ERROR, "Could not find state information for mailbox %s\n", vmtmp->mailbox);
					}
					if (!ast_strlen_zero(vmtmp->serveremail))
						myserveremail = vmtmp->serveremail;
					attach_user_voicemail = ast_test_flag(vmtmp, VM_ATTACH);
					/* NULL category for IMAP storage */
					sendmail(myserveremail, vmtmp, todircount, vmtmp->context, vmtmp->mailbox, S_OR(chan->cid.cid_num, NULL), S_OR(chan->cid.cid_name, NULL), vms->fn, fmt, duration, attach_user_voicemail, chan, NULL);
#else
					copy_message(chan, sender, -1, curmsg, duration, vmtmp, fmt, vmstmp.curdir);
#endif
					saved_messages++;
					AST_LIST_REMOVE_CURRENT(list);
					free_user(vmtmp);
					if (res)
						break;
				}
				AST_LIST_TRAVERSE_SAFE_END;
				if (saved_messages > 0) {
					/* give confirmation that the message was saved */
					/* commented out since we can't forward batches yet
					if (saved_messages == 1)
						res = ast_play_and_wait(chan, "vm-message");
					else
						res = ast_play_and_wait(chan, "vm-messages");
					if (!res)
						res = ast_play_and_wait(chan, "vm-saved"); */
					res = ast_play_and_wait(chan, "vm-msgsaved");
				}	
			}
			/* Remove surrogate file */
			DISPOSE(dir, curmsg);
		}

		/* If anything failed above, we still have this list to free */
		while ((vmtmp = AST_LIST_REMOVE_HEAD(&extensions, list)))
			free_user(vmtmp);
		return res ? res : cmd;
	}

	static int wait_file2(struct ast_channel *chan, struct vm_state *vms, char *file)
	{
		int res;
		if ((res = ast_stream_and_wait(chan, file, AST_DIGIT_ANY)) < 0) 
			ast_log(LOG_WARNING, "Unable to play message %s\n", file); 
		return res;
	}

	static int wait_file(struct ast_channel *chan, struct vm_state *vms, char *file) 
	{
		return ast_control_streamfile(chan, file, listen_control_forward_key, listen_control_reverse_key, listen_control_stop_key, listen_control_pause_key, listen_control_restart_key, skipms, NULL);
	}

	static int play_message_category(struct ast_channel *chan, const char *category)
	{
		int res = 0;

		if (!ast_strlen_zero(category))
			res = ast_play_and_wait(chan, category);

		if (res) {
			ast_log(LOG_WARNING, "No sound file for category '%s' was found.\n", category);
			res = 0;
		}

		return res;
	}

	static int play_message_datetime(struct ast_channel *chan, struct ast_vm_user *vmu, const char *origtime, const char *filename)
	{
		int res = 0;
		struct vm_zone *the_zone = NULL;
		time_t t;

		if (ast_get_time_t(origtime, &t, 0, NULL)) {
			ast_log(LOG_WARNING, "Couldn't find origtime in %s\n", filename);
			return 0;
		}

		/* Does this user have a timezone specified? */
		if (!ast_strlen_zero(vmu->zonetag)) {
			/* Find the zone in the list */
			struct vm_zone *z;
			AST_LIST_LOCK(&zones);
			AST_LIST_TRAVERSE(&zones, z, list) {
				if (!strcmp(z->name, vmu->zonetag)) {
					the_zone = z;
					break;
				}
			}
			AST_LIST_UNLOCK(&zones);
		}

	/* No internal variable parsing for now, so we'll comment it out for the time being */
#if 0
		/* Set the DIFF_* variables */
		ast_localtime(&t, &time_now, NULL);
		tv_now = ast_tvnow();
		ast_localtime(&tv_now, &time_then, NULL);

		/* Day difference */
		if (time_now.tm_year == time_then.tm_year)
			snprintf(temp, sizeof(temp), "%d", time_now.tm_yday);
		else
			snprintf(temp, sizeof(temp), "%d", (time_now.tm_year - time_then.tm_year) * 365 + (time_now.tm_yday - time_then.tm_yday));
		pbx_builtin_setvar_helper(chan, "DIFF_DAY", temp);

		/* Can't think of how other diffs might be helpful, but I'm sure somebody will think of something. */
#endif
		if (the_zone) {
			res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, the_zone->msg_format, the_zone->timezone);
		}
		else if (!strcasecmp(chan->language,"pl"))       /* POLISH syntax */
			res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' Q HM", NULL);
		else if (!strcasecmp(chan->language, "se"))       /* SWEDISH syntax */
			res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' dB 'digits/at' k 'and' M", NULL);
		else if (!strcasecmp(chan->language, "no"))       /* NORWEGIAN syntax */
			res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' Q 'digits/at' HM", NULL);
		else if (!strcasecmp(chan->language, "de"))       /* GERMAN syntax */
			res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' Q 'digits/at' HM", NULL);
		else if (!strcasecmp(chan->language, "nl"))      /* DUTCH syntax */
			res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' q 'digits/nl-om' HM", NULL);
		else if (!strcasecmp(chan->language, "it"))      /* ITALIAN syntax */
			res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' q 'digits/at' 'digits/hours' k 'digits/e' M 'digits/minutes'", NULL);
		else if (!strcasecmp(chan->language, "gr"))
			res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' q  H 'digits/kai' M ", NULL);
		else if (!strcasecmp(chan->language, "pt_BR"))
			res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' Ad 'digits/pt-de' B 'digits/pt-de' Y 'digits/pt-as' HM ", NULL);
		else if (!strcasecmp(chan->language, "tw"))      /* CHINESE (Taiwan) syntax */
			res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "qR 'vm-received'", NULL);		
		else {
			res = ast_say_date_with_format(chan, t, AST_DIGIT_ANY, chan->language, "'vm-received' q 'digits/at' IMp", NULL);
		}
#if 0
		pbx_builtin_setvar_helper(chan, "DIFF_DAY", NULL);
#endif
		return res;
	}



	static int play_message_callerid(struct ast_channel *chan, struct vm_state *vms, char *cid, const char *context, int callback)
	{
		int res = 0;
		int i;
		char *callerid, *name;
		char prefile[PATH_MAX] = "";
		

		/* If voicemail cid is not enabled, or we didn't get cid or context from
		* the attribute file, leave now.
		*
		* TODO Still need to change this so that if this function is called by the
		* message envelope (and someone is explicitly requesting to hear the CID),
		* it does not check to see if CID is enabled in the config file.
		*/
		if ((cid == NULL)||(context == NULL))
			return res;

		/* Strip off caller ID number from name */
		ast_debug(1, "VM-CID: composite caller ID received: %s, context: %s\n", cid, context);
		ast_callerid_parse(cid, &name, &callerid);
		if ((!ast_strlen_zero(callerid)) && strcmp(callerid, "Unknown")) {
			/* Check for internal contexts and only */
			/* say extension when the call didn't come from an internal context in the list */
			for (i = 0 ; i < MAX_NUM_CID_CONTEXTS ; i++) {
				ast_debug(1, "VM-CID: comparing internalcontext: %s\n", cidinternalcontexts[i]);
				if ((strcmp(cidinternalcontexts[i], context) == 0))
					break;
			}
			if (i != MAX_NUM_CID_CONTEXTS) { /* internal context? */
				if (!res) {
					snprintf(prefile, sizeof(prefile), "%s%s/%s/greet", VM_SPOOL_DIR, context, callerid);
					if (!ast_strlen_zero(prefile)) {
					/* See if we can find a recorded name for this person instead of their extension number */
						if (ast_fileexists(prefile, NULL, NULL) > 0) {
							ast_verb(3, "Playing envelope info: CID number '%s' matches mailbox number, playing recorded name\n", callerid);
							if (!callback)
								res = wait_file2(chan, vms, "vm-from");
							res = ast_stream_and_wait(chan, prefile, "");
						} else {
							ast_verb(3, "Playing envelope info: message from '%s'\n", callerid);
							/* Say "from extension" as one saying to sound smoother */
							if (!callback)
								res = wait_file2(chan, vms, "vm-from-extension");
							res = ast_say_digit_str(chan, callerid, "", chan->language);
						}
					}
				}
			} else if (!res) {
				ast_debug(1, "VM-CID: Numeric caller id: (%s)\n", callerid);
				/* Since this is all nicely figured out, why not say "from phone number" in this case? */
				if (!callback)
					res = wait_file2(chan, vms, "vm-from-phonenumber");
				res = ast_say_digit_str(chan, callerid, AST_DIGIT_ANY, chan->language);
			}
		} else {
			/* Number unknown */
			ast_debug(1, "VM-CID: From an unknown number\n");
			/* Say "from an unknown caller" as one phrase - it is already recorded by "the voice" anyhow */
			res = wait_file2(chan, vms, "vm-unknown-caller");
		}
		return res;
	}

	static int play_message_duration(struct ast_channel *chan, struct vm_state *vms, const char *duration, int minduration)
	{
		int res = 0;
		int durationm;
		int durations;
		/* Verify that we have a duration for the message */
		if (duration == NULL)
			return res;

		/* Convert from seconds to minutes */
		durations = atoi(duration);
		durationm = durations / 60;

		ast_debug(1, "VM-Duration: duration is: %d seconds converted to: %d minutes\n", durations, durationm);

		if ((!res) && (durationm >= minduration)) {
			res = wait_file2(chan, vms, "vm-duration");

			/* POLISH syntax */
			if (!strcasecmp(chan->language, "pl")) {
				div_t num = div(durationm, 10);

				if (durationm == 1) {
					res = ast_play_and_wait(chan, "digits/1z");
					res = res ? res : ast_play_and_wait(chan, "vm-minute-ta");
				} else if (num.rem > 1 && num.rem < 5 && num.quot != 1) {
					if (num.rem == 2) {
						if (!num.quot) {
							res = ast_play_and_wait(chan, "digits/2-ie");
						} else {
							res = say_and_wait(chan, durationm - 2 , chan->language);
							res = res ? res : ast_play_and_wait(chan, "digits/2-ie");
						}
					} else {
						res = say_and_wait(chan, durationm, chan->language);
					}
					res = res ? res : ast_play_and_wait(chan, "vm-minute-ty");
				} else {
					res = say_and_wait(chan, durationm, chan->language);
					res = res ? res : ast_play_and_wait(chan, "vm-minute-t");
				}
			/* DEFAULT syntax */
			} else {
				res = ast_say_number(chan, durationm, AST_DIGIT_ANY, chan->language, NULL);
				res = wait_file2(chan, vms, "vm-minutes");
			}
		}
		return res;
	}

	static int play_message(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms)
	{
		int res = 0;
		char filename[256], *cid;
		const char *origtime, *context, *category, *duration;
		struct ast_config *msg_cfg;
		struct ast_flags config_flags = { CONFIG_FLAG_NOCACHE };

		vms->starting = 0; 
		make_file(vms->fn, sizeof(vms->fn), vms->curdir, vms->curmsg);
		adsi_message(chan, vms);
		if (!vms->curmsg)
			res = wait_file2(chan, vms, "vm-first");	/* "First" */
		else if (vms->curmsg == vms->lastmsg)
			res = wait_file2(chan, vms, "vm-last");		/* "last" */
		if (!res) {
			/* POLISH syntax */
			if (!strcasecmp(chan->language, "pl")) { 
				if (vms->curmsg && (vms->curmsg != vms->lastmsg)) {
					int ten, one;
					char nextmsg[256];
					ten = (vms->curmsg + 1) / 10;
					one = (vms->curmsg + 1) % 10;
					
					if (vms->curmsg < 20) {
						snprintf(nextmsg, sizeof(nextmsg), "digits/n-%d", vms->curmsg + 1);
						res = wait_file2(chan, vms, nextmsg);
					} else {
						snprintf(nextmsg, sizeof(nextmsg), "digits/n-%d", ten * 10);
						res = wait_file2(chan, vms, nextmsg);
						if (one > 0) {
							if (!res) {
								snprintf(nextmsg, sizeof(nextmsg), "digits/n-%d", one);
								res = wait_file2(chan, vms, nextmsg);
							}
						}
					}
				}
				if (!res)
					res = wait_file2(chan, vms, "vm-message");
			} else {
				if (!strcasecmp(chan->language, "se")) /* SWEDISH syntax */
					res = wait_file2(chan, vms, "vm-meddelandet");  /* "message" */
				else /* DEFAULT syntax */ {
					res = wait_file2(chan, vms, "vm-message");
				}
				if (vms->curmsg && (vms->curmsg != vms->lastmsg)) {
					if (!res) {
						res = ast_say_number(chan, vms->curmsg + 1, AST_DIGIT_ANY, chan->language, NULL);
					}
				}
			}
		}

		/* Retrieve info from VM attribute file */
		make_file(vms->fn2, sizeof(vms->fn2), vms->curdir, vms->curmsg);
		snprintf(filename, sizeof(filename), "%s.txt", vms->fn2);
		RETRIEVE(vms->curdir, vms->curmsg, vmu->mailbox, vmu->context);
		msg_cfg = ast_config_load(filename, config_flags);
		if (!msg_cfg) {
			ast_log(LOG_WARNING, "No message attribute file?!! (%s)\n", filename);
			return 0;
		}

		if (!(origtime = ast_variable_retrieve(msg_cfg, "message", "origtime"))) {
			ast_log(LOG_WARNING, "No origtime?!\n");
			DISPOSE(vms->curdir, vms->curmsg);
			ast_config_destroy(msg_cfg);
			return 0;
		}

		cid = ast_strdupa(ast_variable_retrieve(msg_cfg, "message", "callerid"));
		duration = ast_variable_retrieve(msg_cfg, "message", "duration");
		category = ast_variable_retrieve(msg_cfg, "message", "category");

		context = ast_variable_retrieve(msg_cfg, "message", "context");
		if (!strncasecmp("macro", context, 5)) /* Macro names in contexts are useless for our needs */
			context = ast_variable_retrieve(msg_cfg, "message", "macrocontext");
		if (!res) {
			res = play_message_category(chan, category);
		}
		if ((!res) && (ast_test_flag(vmu, VM_ENVELOPE)))
			res = play_message_datetime(chan, vmu, origtime, filename);
		if ((!res) && (ast_test_flag(vmu, VM_SAYCID)))
			res = play_message_callerid(chan, vms, cid, context, 0);
		if ((!res) && (ast_test_flag(vmu, VM_SAYDURATION)))
			res = play_message_duration(chan, vms, duration, vmu->saydurationm);
		/* Allow pressing '1' to skip envelope / callerid */
		if (res == '1')
			res = 0;
		ast_config_destroy(msg_cfg);

		if (!res) {
			make_file(vms->fn, sizeof(vms->fn), vms->curdir, vms->curmsg);
			vms->heard[vms->curmsg] = 1;
			if ((res = wait_file(chan, vms, vms->fn)) < 0) {
				ast_log(LOG_WARNING, "Playback of message %s failed\n", vms->fn);
				res = 0;
			}
		}
		DISPOSE(vms->curdir, vms->curmsg);
		return res;
	}

#ifdef IMAP_STORAGE
static int imap_remove_file(char *dir, int msgnum)
{
	char fn[PATH_MAX];
	char full_fn[PATH_MAX];
	
	if (msgnum > -1) {
		make_file(fn, sizeof(fn), dir, msgnum);
	} else
		ast_copy_string(fn, dir, sizeof(fn));
	
	if ((msgnum < 0 && imapgreetings) || msgnum > -1) {
		ast_filedelete(fn, NULL);
		snprintf(full_fn, sizeof(full_fn), "%s.txt", fn);
		unlink(full_fn);
	}
	return 0;
}

static int imap_delete_old_greeting (char *dir, struct vm_state *vms)
{
	char *file, *filename;
	char *attachment;
	char arg[10];
	int i;
	BODY* body;

	
	file = strrchr(ast_strdupa(dir), '/');
	if (file)
		*file++ = '\0';
	else {
		ast_log(LOG_ERROR, "Failed to procure file name from directory passed. You should never see this.\n");
		return -1;
	}
	
	for (i = 0; i < vms->mailstream->nmsgs; i++) {
		mail_fetchstructure(vms->mailstream, i + 1, &body);
		/* We have the body, now we extract the file name of the first attachment. */
		if (body->nested.part->next && body->nested.part->next->body.parameter->value) {
			attachment = ast_strdupa(body->nested.part->next->body.parameter->value);
		} else {
			ast_log(LOG_ERROR, "There is no file attached to this IMAP message.\n");
			return -1;
		}
		filename = strsep(&attachment, ".");
		if (!strcmp(filename, file)) {
			sprintf (arg, "%d", i + 1);
			mail_setflag(vms->mailstream, arg, "\\DELETED");
		}
	}
	mail_expunge(vms->mailstream);
	return 0;
}

#else
#ifndef IMAP_STORAGE
static int open_mailbox(struct vm_state *vms, struct ast_vm_user *vmu, int box)
{
	int res = 0;
	int count_msg, last_msg;

	ast_copy_string(vms->curbox, mbox(box), sizeof(vms->curbox));
	
	/* Rename the member vmbox HERE so that we don't try to return before
	 * we know what's going on.
	 */
	snprintf(vms->vmbox, sizeof(vms->vmbox), "vm-%s", vms->curbox);
	
	/* Faster to make the directory than to check if it exists. */
	create_dirpath(vms->curdir, sizeof(vms->curdir), vmu->context, vms->username, vms->curbox);

	count_msg = count_messages(vmu, vms->curdir);
	if (count_msg < 0)
		return count_msg;
	else
		vms->lastmsg = count_msg - 1;

	/*
	The following test is needed in case sequencing gets messed up.
	There appears to be more than one way to mess up sequence, so
	we will not try to find all of the root causes--just fix it when
	detected.
	*/

	if (vm_lock_path(vms->curdir)) {
		ast_log(LOG_ERROR, "Could not open mailbox %s:  mailbox is locked\n", vms->curdir);
		return -1;
	}

	last_msg = last_message_index(vmu, vms->curdir);
	ast_unlock_path(vms->curdir);

	if (last_msg < 0) 
		return last_msg;
	else if (vms->lastmsg != last_msg) {
		ast_log(LOG_NOTICE, "Resequencing Mailbox: %s\n", vms->curdir);
		res = resequence_mailbox(vmu, vms->curdir);
		if (res)
			return res;
	}

	return 0;
}
#endif
#endif

static int close_mailbox(struct vm_state *vms, struct ast_vm_user *vmu)
{
	int x = 0;
#ifndef IMAP_STORAGE
	int res = 0, nummsg;
#endif

	if (vms->lastmsg <= -1)
		goto done;

	vms->curmsg = -1; 
#ifndef IMAP_STORAGE
	/* Get the deleted messages fixed */ 
	if (vm_lock_path(vms->curdir))
		return ERROR_LOCK_PATH;

	for (x = 0; x < vmu->maxmsg; x++) { 
		if (!vms->deleted[x] && (strcasecmp(vms->curbox, "INBOX") || !vms->heard[x] || (vms->heard[x] && !ast_test_flag(vmu, VM_MOVEHEARD)))) { 
			/* Save this message.  It's not in INBOX or hasn't been heard */ 
			make_file(vms->fn, sizeof(vms->fn), vms->curdir, x); 
			if (!EXISTS(vms->curdir, x, vms->fn, NULL)) 
				break;
			vms->curmsg++; 
			make_file(vms->fn2, sizeof(vms->fn2), vms->curdir, vms->curmsg); 
			if (strcmp(vms->fn, vms->fn2)) { 
				RENAME(vms->curdir, x, vmu->mailbox, vmu->context, vms->curdir, vms->curmsg, vms->fn, vms->fn2);
			} 
		} else if (!strcasecmp(vms->curbox, "INBOX") && vms->heard[x] && ast_test_flag(vmu, VM_MOVEHEARD) && !vms->deleted[x]) { 
			/* Move to old folder before deleting */ 
			res = save_to_folder(vmu, vms, x, 1);
			if (res == ERROR_LOCK_PATH || res == ERROR_MAILBOX_FULL) {
				/* If save failed do not delete the message */
				ast_log(LOG_WARNING, "Save failed.  Not moving message: %s.\n", res == ERROR_LOCK_PATH ? "unable to lock path" : "destination folder full");
				vms->deleted[x] = 0;
				vms->heard[x] = 0;
				--x;
			}
		} else if (vms->deleted[x] && vmu->maxdeletedmsg) {
			/* Move to deleted folder */ 
			res = save_to_folder(vmu, vms, x, 10);
			if (res == ERROR_LOCK_PATH) {
				/* If save failed do not delete the message */
				vms->deleted[x] = 0;
				vms->heard[x] = 0;
				--x;
			}
		} else if (vms->deleted[x] && ast_check_realtime("voicemail_data")) {
			/* If realtime storage enabled - we should explicitly delete this message,
			cause RENAME() will overwrite files, but will keep duplicate records in RT-storage */
			make_file(vms->fn, sizeof(vms->fn), vms->curdir, x);
			if (EXISTS(vms->curdir, x, vms->fn, NULL))
				DELETE(vms->curdir, x, vms->fn, vmu);
		}
	} 

	/* Delete ALL remaining messages */
	nummsg = x - 1;
	for (x = vms->curmsg + 1; x <= nummsg; x++) {
		make_file(vms->fn, sizeof(vms->fn), vms->curdir, x);
		if (EXISTS(vms->curdir, x, vms->fn, NULL))
			DELETE(vms->curdir, x, vms->fn, vmu);
	}
	ast_unlock_path(vms->curdir);
#else
	if (vms->deleted) {
		for (x = 0; x < vmu->maxmsg; x++) { 
			if (vms->deleted[x]) { 
				ast_debug(3, "IMAP delete of %d\n", x);
				DELETE(vms->curdir, x, vms->fn, vmu);
			}
		}
	}
#endif

done:
	if (vms->deleted)
		memset(vms->deleted, 0, vmu->maxmsg * sizeof(int)); 
	if (vms->heard)
		memset(vms->heard, 0, vmu->maxmsg * sizeof(int)); 

	return 0;
}

/* In Greek even though we CAN use a syntax like "friends messages"
 * ("filika mynhmata") it is not elegant. This also goes for "work/family messages"
 * ("ergasiaka/oikogeniaka mynhmata"). Therefore it is better to use a reversed 
 * syntax for the above three categories which is more elegant. 
 */

static int vm_play_folder_name_gr(struct ast_channel *chan, char *mbox)
{
	int cmd;
	char *buf;

	buf = alloca(strlen(mbox) + 2); 
	strcpy(buf, mbox);
	strcat(buf, "s");

	if (!strcasecmp(mbox, "vm-INBOX") || !strcasecmp(mbox, "vm-Old")) {
		cmd = ast_play_and_wait(chan, buf); /* "NEA / PALIA" */
		return cmd ? cmd : ast_play_and_wait(chan, "vm-messages"); /* "messages" -> "MYNHMATA" */
	} else {
		cmd = ast_play_and_wait(chan, "vm-messages"); /* "messages" -> "MYNHMATA" */
		return cmd ? cmd : ast_play_and_wait(chan, mbox); /* friends/family/work... -> "FILWN"/"OIKOGENIAS"/"DOULEIAS"*/
	}
}

static int vm_play_folder_name_pl(struct ast_channel *chan, char *mbox)
{
	int cmd;

	if (!strcasecmp(mbox, "vm-INBOX") || !strcasecmp(mbox, "vm-Old")) {
		if (!strcasecmp(mbox, "vm-INBOX"))
			cmd = ast_play_and_wait(chan, "vm-new-e");
		else
			cmd = ast_play_and_wait(chan, "vm-old-e");
		return cmd ? cmd : ast_play_and_wait(chan, "vm-messages");
	} else {
		cmd = ast_play_and_wait(chan, "vm-messages");
		return cmd ? cmd : ast_play_and_wait(chan, mbox);
	}
}

static int vm_play_folder_name_ua(struct ast_channel *chan, char *mbox)
{
	int cmd;

	if (!strcasecmp(mbox, "vm-Family") || !strcasecmp(mbox, "vm-Friends") || !strcasecmp(mbox, "vm-Work")) {
		cmd = ast_play_and_wait(chan, "vm-messages");
		return cmd ? cmd : ast_play_and_wait(chan, mbox);
	} else {
		cmd = ast_play_and_wait(chan, mbox);
		return cmd ? cmd : ast_play_and_wait(chan, "vm-messages");
	}
}

static int vm_play_folder_name(struct ast_channel *chan, char *mbox)
{
	int cmd;

	if (!strcasecmp(chan->language, "it") || !strcasecmp(chan->language, "es") || !strcasecmp(chan->language, "pt") || !strcasecmp(chan->language, "pt_BR")) { /* Italian, Spanish, French or Portuguese syntax */
		cmd = ast_play_and_wait(chan, "vm-messages"); /* "messages */
		return cmd ? cmd : ast_play_and_wait(chan, mbox);
	} else if (!strcasecmp(chan->language, "gr")) {
		return vm_play_folder_name_gr(chan, mbox);
	} else if (!strcasecmp(chan->language, "pl")) {
		return vm_play_folder_name_pl(chan, mbox);
	} else if (!strcasecmp(chan->language, "ua")) {  /* Ukrainian syntax */
		return vm_play_folder_name_ua(chan, mbox);
	} else {  /* Default English */
		cmd = ast_play_and_wait(chan, mbox);
		return cmd ? cmd : ast_play_and_wait(chan, "vm-messages"); /* "messages */
	}
}

/* GREEK SYNTAX 
	In greek the plural for old/new is
	different so we need the following files
	We also need vm-denExeteMynhmata because 
	this syntax is different.
	
	-> vm-Olds.wav	: "Palia"
	-> vm-INBOXs.wav : "Nea"
	-> vm-denExeteMynhmata : "den exete mynhmata"
*/
					
	
static int vm_intro_gr(struct ast_channel *chan, struct vm_state *vms)
{
	int res = 0;

	if (vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-youhave");
		if (!res) 
			res = ast_say_number(chan, vms->newmessages, AST_DIGIT_ANY, chan->language, NULL);
		if (!res) {
			if ((vms->newmessages == 1)) {
				res = ast_play_and_wait(chan, "vm-INBOX");
				if (!res)
					res = ast_play_and_wait(chan, "vm-message");
			} else {
				res = ast_play_and_wait(chan, "vm-INBOXs");
				if (!res)
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
	} else if (vms->oldmessages) {
		res = ast_play_and_wait(chan, "vm-youhave");
		if (!res)
			res = ast_say_number(chan, vms->oldmessages, AST_DIGIT_ANY, chan->language, NULL);
		if ((vms->oldmessages == 1)) {
			res = ast_play_and_wait(chan, "vm-Old");
			if (!res)
				res = ast_play_and_wait(chan, "vm-message");
		} else {
			res = ast_play_and_wait(chan, "vm-Olds");
			if (!res)
				res = ast_play_and_wait(chan, "vm-messages");
		}
	} else if (!vms->oldmessages && !vms->newmessages) 
		res = ast_play_and_wait(chan, "vm-denExeteMynhmata"); 
	return res;
}
	
/* Default English syntax */
static int vm_intro_en(struct ast_channel *chan, struct vm_state *vms)
{
	int res;

	/* Introduce messages they have */
	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res)
				res = ast_play_and_wait(chan, "vm-INBOX");
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res)
				res = ast_play_and_wait(chan, "vm-Old");
			if (!res) {
				if (vms->oldmessages == 1)
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = ast_play_and_wait(chan, "vm-no");
				if (!res)
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}

/* ITALIAN syntax */
static int vm_intro_it(struct ast_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	if (!vms->oldmessages && !vms->newmessages)
		res =	ast_play_and_wait(chan, "vm-no") ||
			ast_play_and_wait(chan, "vm-message");
	else
		res =	ast_play_and_wait(chan, "vm-youhave");
	if (!res && vms->newmessages) {
		res = (vms->newmessages == 1) ?
			ast_play_and_wait(chan, "digits/un") ||
			ast_play_and_wait(chan, "vm-nuovo") ||
			ast_play_and_wait(chan, "vm-message") :
			/* 2 or more new messages */
			say_and_wait(chan, vms->newmessages, chan->language) ||
			ast_play_and_wait(chan, "vm-nuovi") ||
			ast_play_and_wait(chan, "vm-messages");
		if (!res && vms->oldmessages)
			res =	ast_play_and_wait(chan, "vm-and");
	}
	if (!res && vms->oldmessages) {
		res = (vms->oldmessages == 1) ?
			ast_play_and_wait(chan, "digits/un") ||
			ast_play_and_wait(chan, "vm-vecchio") ||
			ast_play_and_wait(chan, "vm-message") :
			/* 2 or more old messages */
			say_and_wait(chan, vms->oldmessages, chan->language) ||
			ast_play_and_wait(chan, "vm-vecchi") ||
			ast_play_and_wait(chan, "vm-messages");
	}
	return res ? -1 : 0;
}

/* POLISH syntax */
static int vm_intro_pl(struct ast_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	div_t num;

	if (!vms->oldmessages && !vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-no");
		res = res ? res : ast_play_and_wait(chan, "vm-messages");
		return res;
	} else {
		res = ast_play_and_wait(chan, "vm-youhave");
	}

	if (vms->newmessages) {
		num = div(vms->newmessages, 10);
		if (vms->newmessages == 1) {
			res = ast_play_and_wait(chan, "digits/1-a");
			res = res ? res : ast_play_and_wait(chan, "vm-new-a");
			res = res ? res : ast_play_and_wait(chan, "vm-message");
		} else if (num.rem > 1 && num.rem < 5 && num.quot != 1) {
			if (num.rem == 2) {
				if (!num.quot) {
					res = ast_play_and_wait(chan, "digits/2-ie");
				} else {
					res = say_and_wait(chan, vms->newmessages - 2 , chan->language);
					res = res ? res : ast_play_and_wait(chan, "digits/2-ie");
				}
			} else {
				res = say_and_wait(chan, vms->newmessages, chan->language);
			}
			res = res ? res : ast_play_and_wait(chan, "vm-new-e");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		} else {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			res = res ? res : ast_play_and_wait(chan, "vm-new-ych");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		}
		if (!res && vms->oldmessages)
			res = ast_play_and_wait(chan, "vm-and");
	}
	if (!res && vms->oldmessages) {
		num = div(vms->oldmessages, 10);
		if (vms->oldmessages == 1) {
			res = ast_play_and_wait(chan, "digits/1-a");
			res = res ? res : ast_play_and_wait(chan, "vm-old-a");
			res = res ? res : ast_play_and_wait(chan, "vm-message");
		} else if (num.rem > 1 && num.rem < 5 && num.quot != 1) {
			if (num.rem == 2) {
				if (!num.quot) {
					res = ast_play_and_wait(chan, "digits/2-ie");
				} else {
					res = say_and_wait(chan, vms->oldmessages - 2 , chan->language);
					res = res ? res : ast_play_and_wait(chan, "digits/2-ie");
				}
			} else {
				res = say_and_wait(chan, vms->oldmessages, chan->language);
			}
			res = res ? res : ast_play_and_wait(chan, "vm-old-e");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		} else {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			res = res ? res : ast_play_and_wait(chan, "vm-old-ych");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		}
	}

	return res;
}

/* SWEDISH syntax */
static int vm_intro_se(struct ast_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;

	res = ast_play_and_wait(chan, "vm-youhave");
	if (res)
		return res;

	if (!vms->oldmessages && !vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-no");
		res = res ? res : ast_play_and_wait(chan, "vm-messages");
		return res;
	}

	if (vms->newmessages) {
		if ((vms->newmessages == 1)) {
			res = ast_play_and_wait(chan, "digits/ett");
			res = res ? res : ast_play_and_wait(chan, "vm-nytt");
			res = res ? res : ast_play_and_wait(chan, "vm-message");
		} else {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			res = res ? res : ast_play_and_wait(chan, "vm-nya");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		}
		if (!res && vms->oldmessages)
			res = ast_play_and_wait(chan, "vm-and");
	}
	if (!res && vms->oldmessages) {
		if (vms->oldmessages == 1) {
			res = ast_play_and_wait(chan, "digits/ett");
			res = res ? res : ast_play_and_wait(chan, "vm-gammalt");
			res = res ? res : ast_play_and_wait(chan, "vm-message");
		} else {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			res = res ? res : ast_play_and_wait(chan, "vm-gamla");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		}
	}

	return res;
}

/* NORWEGIAN syntax */
static int vm_intro_no(struct ast_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;

	res = ast_play_and_wait(chan, "vm-youhave");
	if (res)
		return res;

	if (!vms->oldmessages && !vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-no");
		res = res ? res : ast_play_and_wait(chan, "vm-messages");
		return res;
	}

	if (vms->newmessages) {
		if ((vms->newmessages == 1)) {
			res = ast_play_and_wait(chan, "digits/1");
			res = res ? res : ast_play_and_wait(chan, "vm-ny");
			res = res ? res : ast_play_and_wait(chan, "vm-message");
		} else {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			res = res ? res : ast_play_and_wait(chan, "vm-nye");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		}
		if (!res && vms->oldmessages)
			res = ast_play_and_wait(chan, "vm-and");
	}
	if (!res && vms->oldmessages) {
		if (vms->oldmessages == 1) {
			res = ast_play_and_wait(chan, "digits/1");
			res = res ? res : ast_play_and_wait(chan, "vm-gamel");
			res = res ? res : ast_play_and_wait(chan, "vm-message");
		} else {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			res = res ? res : ast_play_and_wait(chan, "vm-gamle");
			res = res ? res : ast_play_and_wait(chan, "vm-messages");
		}
	}

	return res;
}

/* GERMAN syntax */
static int vm_intro_de(struct ast_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			if ((vms->newmessages == 1))
				res = ast_play_and_wait(chan, "digits/1F");
			else
				res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res)
				res = ast_play_and_wait(chan, "vm-INBOX");
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			if (vms->oldmessages == 1)
				res = ast_play_and_wait(chan, "digits/1F");
			else
				res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res)
				res = ast_play_and_wait(chan, "vm-Old");
			if (!res) {
				if (vms->oldmessages == 1)
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = ast_play_and_wait(chan, "vm-no");
				if (!res)
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}

/* SPANISH syntax */
static int vm_intro_es(struct ast_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	if (!vms->oldmessages && !vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-youhaveno");
		if (!res)
			res = ast_play_and_wait(chan, "vm-messages");
	} else {
		res = ast_play_and_wait(chan, "vm-youhave");
	}
	if (!res) {
		if (vms->newmessages) {
			if (!res) {
				if ((vms->newmessages == 1)) {
					res = ast_play_and_wait(chan, "digits/1M");
					if (!res)
						res = ast_play_and_wait(chan, "vm-message");
					if (!res)
						res = ast_play_and_wait(chan, "vm-INBOXs");
				} else {
					res = say_and_wait(chan, vms->newmessages, chan->language);
					if (!res)
						res = ast_play_and_wait(chan, "vm-messages");
					if (!res)
						res = ast_play_and_wait(chan, "vm-INBOX");
				}
			}
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
		}
		if (vms->oldmessages) {
			if (!res) {
				if (vms->oldmessages == 1) {
					res = ast_play_and_wait(chan, "digits/1M");
					if (!res)
						res = ast_play_and_wait(chan, "vm-message");
					if (!res)
						res = ast_play_and_wait(chan, "vm-Olds");
				} else {
					res = say_and_wait(chan, vms->oldmessages, chan->language);
					if (!res)
						res = ast_play_and_wait(chan, "vm-messages");
					if (!res)
						res = ast_play_and_wait(chan, "vm-Old");
				}
			}
		}
	}
return res;
}

/* BRAZILIAN PORTUGUESE syntax */
static int vm_intro_pt_BR(struct ast_channel *chan, struct vm_state *vms) {
	/* Introduce messages they have */
	int res;
	if (!vms->oldmessages && !vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-nomessages");
		return res;
	} else {
		res = ast_play_and_wait(chan, "vm-youhave");
	}
	if (vms->newmessages) {
		if (!res)
			res = ast_say_number(chan, vms->newmessages, AST_DIGIT_ANY, chan->language, "f");
		if ((vms->newmessages == 1)) {
			if (!res)
				res = ast_play_and_wait(chan, "vm-message");
			if (!res)
				res = ast_play_and_wait(chan, "vm-INBOXs");
		} else {
			if (!res)
				res = ast_play_and_wait(chan, "vm-messages");
			if (!res)
				res = ast_play_and_wait(chan, "vm-INBOX");
		}
		if (vms->oldmessages && !res)
			res = ast_play_and_wait(chan, "vm-and");
	}
	if (vms->oldmessages) {
		if (!res)
			res = ast_say_number(chan, vms->oldmessages, AST_DIGIT_ANY, chan->language, "f");
		if (vms->oldmessages == 1) {
			if (!res)
				res = ast_play_and_wait(chan, "vm-message");
			if (!res)
				res = ast_play_and_wait(chan, "vm-Olds");
		} else {
			if (!res)
				res = ast_play_and_wait(chan, "vm-messages");
			if (!res)
				res = ast_play_and_wait(chan, "vm-Old");
		}
	}
	return res;
}

/* FRENCH syntax */
static int vm_intro_fr(struct ast_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res)
				res = ast_play_and_wait(chan, "vm-INBOX");
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res)
				res = ast_play_and_wait(chan, "vm-Old");
			if (!res) {
				if (vms->oldmessages == 1)
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = ast_play_and_wait(chan, "vm-no");
				if (!res)
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}

/* DUTCH syntax */
static int vm_intro_nl(struct ast_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = say_and_wait(chan, vms->newmessages, chan->language);
			if (!res) {
				if (vms->newmessages == 1)
					res = ast_play_and_wait(chan, "vm-INBOXs");
				else
					res = ast_play_and_wait(chan, "vm-INBOX");
			}
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
				
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res) {
				if (vms->oldmessages == 1)
					res = ast_play_and_wait(chan, "vm-Olds");
				else
					res = ast_play_and_wait(chan, "vm-Old");
			}
			if (!res) {
				if (vms->oldmessages == 1)
					res = ast_play_and_wait(chan, "vm-message");
				else
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = ast_play_and_wait(chan, "vm-no");
				if (!res)
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}

/* PORTUGUESE syntax */
static int vm_intro_pt(struct ast_channel *chan, struct vm_state *vms)
{
	/* Introduce messages they have */
	int res;
	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			res = ast_say_number(chan, vms->newmessages, AST_DIGIT_ANY, chan->language, "f");
			if (!res) {
				if ((vms->newmessages == 1)) {
					res = ast_play_and_wait(chan, "vm-message");
					if (!res)
						res = ast_play_and_wait(chan, "vm-INBOXs");
				} else {
					res = ast_play_and_wait(chan, "vm-messages");
					if (!res)
						res = ast_play_and_wait(chan, "vm-INBOX");
				}
			}
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
		}
		if (!res && vms->oldmessages) {
			res = ast_say_number(chan, vms->oldmessages, AST_DIGIT_ANY, chan->language, "f");
			if (!res) {
				if (vms->oldmessages == 1) {
					res = ast_play_and_wait(chan, "vm-message");
					if (!res)
						res = ast_play_and_wait(chan, "vm-Olds");
				} else {
					res = ast_play_and_wait(chan, "vm-messages");
					if (!res)
						res = ast_play_and_wait(chan, "vm-Old");
				}
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = ast_play_and_wait(chan, "vm-no");
				if (!res)
					res = ast_play_and_wait(chan, "vm-messages");
			}
		}
	}
	return res;
}


/* CZECH syntax */
/* in czech there must be declension of word new and message
 * czech        : english        : czech      : english
 * --------------------------------------------------------
 * vm-youhave   : you have 
 * vm-novou     : one new        : vm-zpravu  : message
 * vm-nove      : 2-4 new        : vm-zpravy  : messages
 * vm-novych    : 5-infinite new : vm-zprav   : messages
 * vm-starou	: one old
 * vm-stare     : 2-4 old 
 * vm-starych   : 5-infinite old
 * jednu        : one	- falling 4. 
 * vm-no        : no  ( no messages )
 */

static int vm_intro_cz(struct ast_channel *chan, struct vm_state *vms)
{
	int res;
	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res) {
		if (vms->newmessages) {
			if (vms->newmessages == 1) {
				res = ast_play_and_wait(chan, "digits/jednu");
			} else {
				res = say_and_wait(chan, vms->newmessages, chan->language);
			}
			if (!res) {
				if ((vms->newmessages == 1))
					res = ast_play_and_wait(chan, "vm-novou");
				if ((vms->newmessages) > 1 && (vms->newmessages < 5))
					res = ast_play_and_wait(chan, "vm-nove");
				if (vms->newmessages > 4)
					res = ast_play_and_wait(chan, "vm-novych");
			}
			if (vms->oldmessages && !res)
				res = ast_play_and_wait(chan, "vm-and");
			else if (!res) {
				if ((vms->newmessages == 1))
					res = ast_play_and_wait(chan, "vm-zpravu");
				if ((vms->newmessages) > 1 && (vms->newmessages < 5))
					res = ast_play_and_wait(chan, "vm-zpravy");
				if (vms->newmessages > 4)
					res = ast_play_and_wait(chan, "vm-zprav");
			}
		}
		if (!res && vms->oldmessages) {
			res = say_and_wait(chan, vms->oldmessages, chan->language);
			if (!res) {
				if ((vms->oldmessages == 1))
					res = ast_play_and_wait(chan, "vm-starou");
				if ((vms->oldmessages) > 1 && (vms->oldmessages < 5))
					res = ast_play_and_wait(chan, "vm-stare");
				if (vms->oldmessages > 4)
					res = ast_play_and_wait(chan, "vm-starych");
			}
			if (!res) {
				if ((vms->oldmessages == 1))
					res = ast_play_and_wait(chan, "vm-zpravu");
				if ((vms->oldmessages) > 1 && (vms->oldmessages < 5))
					res = ast_play_and_wait(chan, "vm-zpravy");
				if (vms->oldmessages > 4)
					res = ast_play_and_wait(chan, "vm-zprav");
			}
		}
		if (!res) {
			if (!vms->oldmessages && !vms->newmessages) {
				res = ast_play_and_wait(chan, "vm-no");
				if (!res)
					res = ast_play_and_wait(chan, "vm-zpravy");
			}
		}
	}
	return res;
}

static int get_lastdigits(int num)
{
	num %= 100;
	return (num < 20) ? num : num % 10;
}

static int vm_intro_ru(struct ast_channel *chan, struct vm_state *vms)
{
	int res;
	int lastnum = 0;
	int dcnum;

	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res && vms->newmessages) {
		lastnum = get_lastdigits(vms->newmessages);
		dcnum = vms->newmessages - lastnum;
		if (dcnum)
			res = say_and_wait(chan, dcnum, chan->language);
		if (!res && lastnum) {
			if (lastnum == 1) 
				res = ast_play_and_wait(chan, "digits/odno");
			else
				res = say_and_wait(chan, lastnum, chan->language);
		}

		if (!res)
			res = ast_play_and_wait(chan, (lastnum == 1) ? "vm-novoe" : "vm-novyh");

		if (!res && vms->oldmessages)
			res = ast_play_and_wait(chan, "vm-and");
	}

	if (!res && vms->oldmessages) {
		lastnum = get_lastdigits(vms->oldmessages);
		dcnum = vms->oldmessages - lastnum;
		if (dcnum)
			res = say_and_wait(chan, dcnum, chan->language);
		if (!res && lastnum) {
			if (lastnum == 1) 
				res = ast_play_and_wait(chan, "digits/odno");
			else
				res = say_and_wait(chan, lastnum, chan->language);
		}

		if (!res)
			res = ast_play_and_wait(chan, (lastnum == 1) ? "vm-staroe" : "vm-staryh");
	}

	if (!res && !vms->newmessages && !vms->oldmessages) {
		lastnum = 0;
		res = ast_play_and_wait(chan, "vm-no");
	}

	if (!res) {
		switch (lastnum) {
		case 1:
			res = ast_play_and_wait(chan, "vm-soobshenie");
			break;
		case 2:
		case 3:
		case 4:
			res = ast_play_and_wait(chan, "vm-soobsheniya");
			break;
		default:
			res = ast_play_and_wait(chan, "vm-soobsheniy");
			break;
		}
	}

	return res;
}

/* CHINESE (Taiwan) syntax */
static int vm_intro_tw(struct ast_channel *chan, struct vm_state *vms)
{
	int res;
	/* Introduce messages they have */
	res = ast_play_and_wait(chan, "vm-you");

	if (!res && vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-have");
		if (!res)
			res = say_and_wait(chan, vms->newmessages, chan->language);
		if (!res)
			res = ast_play_and_wait(chan, "vm-tong");
		if (!res)
			res = ast_play_and_wait(chan, "vm-INBOX");
		if (vms->oldmessages && !res)
			res = ast_play_and_wait(chan, "vm-and");
		else if (!res) 
			res = ast_play_and_wait(chan, "vm-messages");
	}
	if (!res && vms->oldmessages) {
		res = ast_play_and_wait(chan, "vm-have");
		if (!res)
			res = say_and_wait(chan, vms->oldmessages, chan->language);
		if (!res)
			res = ast_play_and_wait(chan, "vm-tong");
		if (!res)
			res = ast_play_and_wait(chan, "vm-Old");
		if (!res)
			res = ast_play_and_wait(chan, "vm-messages");
	}
	if (!res && !vms->oldmessages && !vms->newmessages) {
		res = ast_play_and_wait(chan, "vm-haveno");
		if (!res)
			res = ast_play_and_wait(chan, "vm-messages");
	}
	return res;
}

/* UKRAINIAN syntax */
/* in ukrainian the syntax is different so we need the following files
 * --------------------------------------------------------
 * /digits/ua/1e 'odne'
 * vm-nove       'nove'
 * vm-stare      'stare'
 */
static int vm_intro_ua(struct ast_channel *chan, struct vm_state *vms)
{
	int res;
	int lastnum = 0;
	int dcnum;

	res = ast_play_and_wait(chan, "vm-youhave");
	if (!res && vms->newmessages) {
		lastnum = get_lastdigits(vms->newmessages);
		dcnum = vms->newmessages - lastnum;
		if (dcnum)
			res = say_and_wait(chan, dcnum, chan->language);
		if (!res && lastnum) {
			if (lastnum == 1) 
				res = ast_play_and_wait(chan, "digits/ua/1e");
			else
				res = say_and_wait(chan, lastnum, chan->language);
		}

		if (!res)
			res = ast_play_and_wait(chan, (lastnum == 1) ? "vm-nove" : "vm-INBOX");

		if (!res && vms->oldmessages)
			res = ast_play_and_wait(chan, "vm-and");
	}

	if (!res && vms->oldmessages) {
		lastnum = get_lastdigits(vms->oldmessages);
		dcnum = vms->oldmessages - lastnum;
		if (dcnum)
			res = say_and_wait(chan, dcnum, chan->language);
		if (!res && lastnum) {
			if (lastnum == 1) 
				res = ast_play_and_wait(chan, "digits/ua/1e");
			else
				res = say_and_wait(chan, lastnum, chan->language);
		}

		if (!res)
			res = ast_play_and_wait(chan, (lastnum == 1) ? "vm-stare" : "vm-Old");
	}

	if (!res && !vms->newmessages && !vms->oldmessages) {
		lastnum = 0;
		res = ast_play_and_wait(chan, "vm-no");
	}

	if (!res) {
		switch (lastnum) {
		case 1:
		case 2:
		case 3:
		case 4:
			res = ast_play_and_wait(chan, "vm-message");
			break;
		default:
			res = ast_play_and_wait(chan, "vm-messages");
			break;
		}
	}

	return res;
}

static int vm_intro(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms)
{
	char prefile[256];
	
	/* Notify the user that the temp greeting is set and give them the option to remove it */
	snprintf(prefile, sizeof(prefile), "%s%s/%s/temp", VM_SPOOL_DIR, vmu->context, vms->username);
	if (ast_test_flag(vmu, VM_TEMPGREETWARN)) {
		if (ast_fileexists(prefile, NULL, NULL) > 0)
			ast_play_and_wait(chan, "vm-tempgreetactive");
	}

	/* Play voicemail intro - syntax is different for different languages */
	if (!strcasecmp(chan->language, "de")) {	/* GERMAN syntax */
		return vm_intro_de(chan, vms);
	} else if (!strcasecmp(chan->language, "es")) { /* SPANISH syntax */
		return vm_intro_es(chan, vms);
	} else if (!strcasecmp(chan->language, "it")) { /* ITALIAN syntax */
		return vm_intro_it(chan, vms);
	} else if (!strcasecmp(chan->language, "fr")) {	/* FRENCH syntax */
		return vm_intro_fr(chan, vms);
	} else if (!strcasecmp(chan->language, "nl")) {	/* DUTCH syntax */
		return vm_intro_nl(chan, vms);
	} else if (!strcasecmp(chan->language, "pt")) {	/* PORTUGUESE syntax */
		return vm_intro_pt(chan, vms);
	} else if (!strcasecmp(chan->language, "pt_BR")) {	/* BRAZILIAN PORTUGUESE syntax */
		return vm_intro_pt_BR(chan, vms);
	} else if (!strcasecmp(chan->language, "cz")) {	/* CZECH syntax */
		return vm_intro_cz(chan, vms);
	} else if (!strcasecmp(chan->language, "gr")) {	/* GREEK syntax */
		return vm_intro_gr(chan, vms);
	} else if (!strcasecmp(chan->language, "pl")) {	/* POLISH syntax */
		return vm_intro_pl(chan, vms);
	} else if (!strcasecmp(chan->language, "se")) {	/* SWEDISH syntax */
		return vm_intro_se(chan, vms);
	} else if (!strcasecmp(chan->language, "no")) {	/* NORWEGIAN syntax */
		return vm_intro_no(chan, vms);
	} else if (!strcasecmp(chan->language, "ru")) { /* RUSSIAN syntax */
		return vm_intro_ru(chan, vms);
	} else if (!strcasecmp(chan->language, "tw")) { /* CHINESE (Taiwan) syntax */
		return vm_intro_tw(chan, vms);
	} else if (!strcasecmp(chan->language, "ua")) { /* UKRAINIAN syntax */
		return vm_intro_ua(chan, vms);
	} else {					/* Default to ENGLISH */
		return vm_intro_en(chan, vms);
	}
}

static int vm_instructions_en(struct ast_channel *chan, struct vm_state *vms, int skipadvanced)
{
	int res = 0;
	/* Play instructions and wait for new command */
	while (!res) {
		if (vms->starting) {
			if (vms->lastmsg > -1) {
				res = ast_play_and_wait(chan, "vm-onefor");
				if (!res)
					res = vm_play_folder_name(chan, vms->vmbox);
			}
			if (!res)
				res = ast_play_and_wait(chan, "vm-opts");
		} else {
			if (vms->curmsg)
				res = ast_play_and_wait(chan, "vm-prev");
			if (!res && !skipadvanced)
				res = ast_play_and_wait(chan, "vm-advopts");
			if (!res)
				res = ast_play_and_wait(chan, "vm-repeat");
			if (!res && (vms->curmsg != vms->lastmsg))
				res = ast_play_and_wait(chan, "vm-next");
			if (!res) {
				if (!vms->deleted[vms->curmsg])
					res = ast_play_and_wait(chan, "vm-delete");
				else
					res = ast_play_and_wait(chan, "vm-undelete");
				if (!res)
					res = ast_play_and_wait(chan, "vm-toforward");
				if (!res)
					res = ast_play_and_wait(chan, "vm-savemessage");
			}
		}
		if (!res)
			res = ast_play_and_wait(chan, "vm-helpexit");
		if (!res)
			res = ast_waitfordigit(chan, 6000);
		if (!res) {
			vms->repeats++;
			if (vms->repeats > 2) {
				res = 't';
			}
		}
	}
	return res;
}

static int vm_instructions_tw(struct ast_channel *chan, struct vm_state *vms, int skipadvanced)
{
	int res = 0;
	/* Play instructions and wait for new command */
	while (!res) {
		if (vms->lastmsg > -1) {
			res = ast_play_and_wait(chan, "vm-listen");
			if (!res)
				res = vm_play_folder_name(chan, vms->vmbox);
			if (!res)
				res = ast_play_and_wait(chan, "press");
			if (!res)
				res = ast_play_and_wait(chan, "digits/1");
		}
		if (!res)
			res = ast_play_and_wait(chan, "vm-opts");
		if (!res) {
			vms->starting = 0;
			return vm_instructions_en(chan, vms, skipadvanced);
		}
	}
	return res;
}

static int vm_instructions(struct ast_channel *chan, struct vm_state *vms, int skipadvanced)
{
	if (vms->starting && !strcasecmp(chan->language, "tw")) { /* CHINESE (Taiwan) syntax */
		return vm_instructions_tw(chan, vms, skipadvanced);
	} else {					/* Default to ENGLISH */
		return vm_instructions_en(chan, vms, skipadvanced);
	}
}


static int vm_newuser(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain)
{
	int cmd = 0;
	int duration = 0;
	int tries = 0;
	char newpassword[80] = "";
	char newpassword2[80] = "";
	char prefile[PATH_MAX] = "";
	unsigned char buf[256];
	int bytes = 0;

	if (ast_adsi_available(chan)) {
		bytes += adsi_logo(buf + bytes);
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "New User Setup", "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Not Done", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	/* First, have the user change their password 
	   so they won't get here again */
	for (;;) {
		newpassword[1] = '\0';
		newpassword[0] = cmd = ast_play_and_wait(chan, vm_newpassword);
		if (cmd == '#')
			newpassword[0] = '\0';
		if (cmd < 0 || cmd == 't' || cmd == '#')
			return cmd;
		cmd = ast_readstring(chan, newpassword + strlen(newpassword), sizeof(newpassword) - 1, 2000, 10000, "#");
		if (cmd < 0 || cmd == 't' || cmd == '#')
			return cmd;
		newpassword2[1] = '\0';
		newpassword2[0] = cmd = ast_play_and_wait(chan, vm_reenterpassword);
		if (cmd == '#')
			newpassword2[0] = '\0';
		if (cmd < 0 || cmd == 't' || cmd == '#')
			return cmd;
		cmd = ast_readstring(chan, newpassword2 + strlen(newpassword2), sizeof(newpassword2) - 1, 2000, 10000, "#");
		if (cmd < 0 || cmd == 't' || cmd == '#')
			return cmd;
		if (!strcmp(newpassword, newpassword2))
			break;
		ast_log(LOG_NOTICE, "Password mismatch for user %s (%s != %s)\n", vms->username, newpassword, newpassword2);
		cmd = ast_play_and_wait(chan, vm_mismatch);
		if (++tries == 3)
			return -1;
	}
	if (pwdchange & PWDCHANGE_INTERNAL)
		vm_change_password(vmu, newpassword);
	if ((pwdchange & PWDCHANGE_EXTERNAL) && !ast_strlen_zero(ext_pass_cmd))
		vm_change_password_shell(vmu, newpassword);

	ast_debug(1, "User %s set password to %s of length %d\n", vms->username, newpassword, (int)strlen(newpassword));
	cmd = ast_play_and_wait(chan, vm_passchanged);

	/* If forcename is set, have the user record their name */	
	if (ast_test_flag(vmu, VM_FORCENAME)) {
		snprintf(prefile, sizeof(prefile), "%s%s/%s/greet", VM_SPOOL_DIR, vmu->context, vms->username);
		if (ast_fileexists(prefile, NULL, NULL) < 1) {
			cmd = play_record_review(chan, "vm-rec-name", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, vms);
			if (cmd < 0 || cmd == 't' || cmd == '#')
				return cmd;
		}
	}

	/* If forcegreetings is set, have the user record their greetings */
	if (ast_test_flag(vmu, VM_FORCEGREET)) {
		snprintf(prefile, sizeof(prefile), "%s%s/%s/unavail", VM_SPOOL_DIR, vmu->context, vms->username);
		if (ast_fileexists(prefile, NULL, NULL) < 1) {
			cmd = play_record_review(chan, "vm-rec-unv", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, vms);
			if (cmd < 0 || cmd == 't' || cmd == '#')
				return cmd;
		}

		snprintf(prefile, sizeof(prefile), "%s%s/%s/busy", VM_SPOOL_DIR, vmu->context, vms->username);
		if (ast_fileexists(prefile, NULL, NULL) < 1) {
			cmd = play_record_review(chan, "vm-rec-busy", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, vms);
			if (cmd < 0 || cmd == 't' || cmd == '#')
				return cmd;
		}
	}

	return cmd;
}

static int vm_options(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain)
{
	int cmd = 0;
	int retries = 0;
	int duration = 0;
	char newpassword[80] = "";
	char newpassword2[80] = "";
	char prefile[PATH_MAX] = "";
	unsigned char buf[256];
	int bytes = 0;

	if (ast_adsi_available(chan)) {
		bytes += adsi_logo(buf + bytes);
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Options Menu", "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Not Done", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}
	while ((cmd >= 0) && (cmd != 't')) {
		if (cmd)
			retries = 0;
		switch (cmd) {
		case '1':
			snprintf(prefile, sizeof(prefile), "%s%s/%s/unavail", VM_SPOOL_DIR, vmu->context, vms->username);
			cmd = play_record_review(chan, "vm-rec-unv", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, vms);
			break;
		case '2': 
			snprintf(prefile, sizeof(prefile), "%s%s/%s/busy", VM_SPOOL_DIR, vmu->context, vms->username);
			cmd = play_record_review(chan, "vm-rec-busy", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, vms);
			break;
		case '3': 
			snprintf(prefile, sizeof(prefile), "%s%s/%s/greet", VM_SPOOL_DIR, vmu->context, vms->username);
			cmd = play_record_review(chan, "vm-rec-name", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, vms);
			break;
		case '4': 
			cmd = vm_tempgreeting(chan, vmu, vms, fmtc, record_gain);
			break;
		case '5':
			if (vmu->password[0] == '-') {
				cmd = ast_play_and_wait(chan, "vm-no");
				break;
			}
			newpassword[1] = '\0';
			newpassword[0] = cmd = ast_play_and_wait(chan, vm_newpassword);
			if (cmd == '#')
				newpassword[0] = '\0';
			else {
				if (cmd < 0)
					break;
				if ((cmd = ast_readstring(chan, newpassword + strlen(newpassword), sizeof(newpassword) - 1, 2000, 10000, "#")) < 0) {
					break;
				}
			}
			newpassword2[1] = '\0';
			newpassword2[0] = cmd = ast_play_and_wait(chan, vm_reenterpassword);
			if (cmd == '#')
				newpassword2[0] = '\0';
			else {
				if (cmd < 0)
					break;

				if ((cmd = ast_readstring(chan, newpassword2 + strlen(newpassword2), sizeof(newpassword2) - 1, 2000, 10000, "#"))) {
					break;
				}
			}
			if (strcmp(newpassword, newpassword2)) {
				ast_log(LOG_NOTICE, "Password mismatch for user %s (%s != %s)\n", vms->username, newpassword, newpassword2);
				cmd = ast_play_and_wait(chan, vm_mismatch);
				break;
			}
			if (pwdchange & PWDCHANGE_INTERNAL)
				vm_change_password(vmu, newpassword);
			if ((pwdchange & PWDCHANGE_EXTERNAL) && !ast_strlen_zero(ext_pass_cmd))
				vm_change_password_shell(vmu, newpassword);

			ast_debug(1, "User %s set password to %s of length %d\n", vms->username, newpassword, (int)strlen(newpassword));
			cmd = ast_play_and_wait(chan, vm_passchanged);
			break;
		case '*': 
			cmd = 't';
			break;
		default: 
			cmd = 0;
			snprintf(prefile, sizeof(prefile), "%s%s/%s/temp", VM_SPOOL_DIR, vmu->context, vms->username);
			if (ast_fileexists(prefile, NULL, NULL)) {
				cmd = ast_play_and_wait(chan, "vm-tmpexists");
			}
			if (!cmd) {
				cmd = ast_play_and_wait(chan, "vm-options");
			}
			if (!cmd) {
				cmd = ast_waitfordigit(chan, 6000);
			}
			if (!cmd) {
				retries++;
			}
			if (retries > 3) {
				cmd = 't';
			}
		}
	}
	if (cmd == 't')
		cmd = 0;
	return cmd;
}

static int vm_tempgreeting(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, char *fmtc, signed char record_gain)
{
	int cmd = 0;
	int retries = 0;
	int duration = 0;
	char prefile[PATH_MAX] = "";
	unsigned char buf[256];
	int bytes = 0;

	if (ast_adsi_available(chan)) {
		bytes += adsi_logo(buf + bytes);
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 3, ADSI_JUST_CENT, 0, "Temp Greeting Menu", "");
		bytes += ast_adsi_display(buf + bytes, ADSI_COMM_PAGE, 4, ADSI_JUST_CENT, 0, "Not Done", "");
		bytes += ast_adsi_set_line(buf + bytes, ADSI_COMM_PAGE, 1);
		bytes += ast_adsi_voice_mode(buf + bytes, 0);
		ast_adsi_transmit_message(chan, buf, bytes, ADSI_MSG_DISPLAY);
	}

	snprintf(prefile, sizeof(prefile), "%s%s/%s/temp", VM_SPOOL_DIR, vmu->context, vms->username);
	while ((cmd >= 0) && (cmd != 't')) {
		if (cmd)
			retries = 0;
		RETRIEVE(prefile, -1, vmu->mailbox, vmu->context);
		if (ast_fileexists(prefile, NULL, NULL) <= 0) {
			play_record_review(chan, "vm-rec-temp", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, vms);
			cmd = 't';	
		} else {
			switch (cmd) {
			case '1':
				cmd = play_record_review(chan, "vm-rec-temp", prefile, maxgreet, fmtc, 0, vmu, &duration, NULL, record_gain, vms);
				break;
			case '2':
				DELETE(prefile, -1, prefile, vmu);
				ast_play_and_wait(chan, "vm-tempremoved");
				cmd = 't';	
				break;
			case '*': 
				cmd = 't';
				break;
			default:
				cmd = ast_play_and_wait(chan,
					ast_fileexists(prefile, NULL, NULL) > 0 ? /* XXX always true ? */
						"vm-tempgreeting2" : "vm-tempgreeting");
				if (!cmd)
					cmd = ast_waitfordigit(chan, 6000);
				if (!cmd)
					retries++;
				if (retries > 3)
					cmd = 't';
			}
		}
		DISPOSE(prefile, -1);
	}
	if (cmd == 't')
		cmd = 0;
	return cmd;
}

/* GREEK SYNTAX */
	
static int vm_browse_messages_gr(struct ast_channel *chan, struct vm_state *vms, struct ast_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = ast_play_and_wait(chan, "vm-youhaveno");
		if (!strcasecmp(vms->vmbox, "vm-INBOX") ||!strcasecmp(vms->vmbox, "vm-Old")) {
			if (!cmd) {
				snprintf(vms->fn, sizeof(vms->fn), "vm-%ss", vms->curbox);
				cmd = ast_play_and_wait(chan, vms->fn);
			}
			if (!cmd)
				cmd = ast_play_and_wait(chan, "vm-messages");
		} else {
			if (!cmd)
				cmd = ast_play_and_wait(chan, "vm-messages");
			if (!cmd) {
				snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
				cmd = ast_play_and_wait(chan, vms->fn);
			}
		}
	} 
	return cmd;
}

/* Default English syntax */
static int vm_browse_messages_en(struct ast_channel *chan, struct vm_state *vms, struct ast_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = ast_play_and_wait(chan, "vm-youhave");
		if (!cmd) 
			cmd = ast_play_and_wait(chan, "vm-no");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
			cmd = ast_play_and_wait(chan, vms->fn);
		}
		if (!cmd)
			cmd = ast_play_and_wait(chan, "vm-messages");
	}
	return cmd;
}

/* ITALIAN syntax */
static int vm_browse_messages_it(struct ast_channel *chan, struct vm_state *vms, struct ast_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = ast_play_and_wait(chan, "vm-no");
		if (!cmd)
			cmd = ast_play_and_wait(chan, "vm-message");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
			cmd = ast_play_and_wait(chan, vms->fn);
		}
	}
	return cmd;
}

/* SPANISH syntax */
static int vm_browse_messages_es(struct ast_channel *chan, struct vm_state *vms, struct ast_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = ast_play_and_wait(chan, "vm-youhaveno");
		if (!cmd)
			cmd = ast_play_and_wait(chan, "vm-messages");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
			cmd = ast_play_and_wait(chan, vms->fn);
		}
	}
	return cmd;
}

/* PORTUGUESE syntax */
static int vm_browse_messages_pt(struct ast_channel *chan, struct vm_state *vms, struct ast_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = ast_play_and_wait(chan, "vm-no");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
			cmd = ast_play_and_wait(chan, vms->fn);
		}
		if (!cmd)
			cmd = ast_play_and_wait(chan, "vm-messages");
	}
	return cmd;
}

/* Chinese (Taiwan)syntax */
static int vm_browse_messages_tw(struct ast_channel *chan, struct vm_state *vms, struct ast_vm_user *vmu)
{
	int cmd = 0;

	if (vms->lastmsg > -1) {
		cmd = play_message(chan, vmu, vms);
	} else {
		cmd = ast_play_and_wait(chan, "vm-you");
		if (!cmd) 
			cmd = ast_play_and_wait(chan, "vm-haveno");
		if (!cmd)
			cmd = ast_play_and_wait(chan, "vm-messages");
		if (!cmd) {
			snprintf(vms->fn, sizeof(vms->fn), "vm-%s", vms->curbox);
			cmd = ast_play_and_wait(chan, vms->fn);
		}
	}
	return cmd;
}

static int vm_browse_messages(struct ast_channel *chan, struct vm_state *vms, struct ast_vm_user *vmu)
{
	if (!strcasecmp(chan->language, "es")) {	/* SPANISH */
		return vm_browse_messages_es(chan, vms, vmu);
	} else if (!strcasecmp(chan->language, "it")) { /* ITALIAN */
		return vm_browse_messages_it(chan, vms, vmu);
	} else if (!strcasecmp(chan->language, "pt") || !strcasecmp(chan->language, "pt_BR")) {	/* PORTUGUESE */
		return vm_browse_messages_pt(chan, vms, vmu);
	} else if (!strcasecmp(chan->language, "gr")) {
		return vm_browse_messages_gr(chan, vms, vmu);   /* GREEK */
	} else if (!strcasecmp(chan->language, "tw")) {
		return vm_browse_messages_tw(chan, vms, vmu);   /* CHINESE (Taiwan) */
	} else {	/* Default to English syntax */
		return vm_browse_messages_en(chan, vms, vmu);
	}
}

static int vm_authenticate(struct ast_channel *chan, char *mailbox, int mailbox_size,
			struct ast_vm_user *res_vmu, const char *context, const char *prefix,
			int skipuser, int maxlogins, int silent)
{
	int useadsi = 0, valid = 0, logretries = 0;
	char password[AST_MAX_EXTENSION] = "", *passptr;
	struct ast_vm_user vmus, *vmu = NULL;

	/* If ADSI is supported, setup login screen */
	adsi_begin(chan, &useadsi);
	if (!skipuser && useadsi)
		adsi_login(chan);
	if (!silent && !skipuser && ast_streamfile(chan, "vm-login", chan->language)) {
		ast_log(LOG_WARNING, "Couldn't stream login file\n");
		return -1;
	}
	
	/* Authenticate them and get their mailbox/password */
	
	while (!valid && (logretries < maxlogins)) {
		/* Prompt for, and read in the username */
		if (!skipuser && ast_readstring(chan, mailbox, mailbox_size - 1, 2000, 10000, "#") < 0) {
			ast_log(LOG_WARNING, "Couldn't read username\n");
			return -1;
		}
		if (ast_strlen_zero(mailbox)) {
			if (chan->cid.cid_num) {
				ast_copy_string(mailbox, chan->cid.cid_num, mailbox_size);
			} else {
				ast_verb(3, "Username not entered\n");	
				return -1;
			}
		}
		if (useadsi)
			adsi_password(chan);

		if (!ast_strlen_zero(prefix)) {
			char fullusername[80] = "";
			ast_copy_string(fullusername, prefix, sizeof(fullusername));
			strncat(fullusername, mailbox, sizeof(fullusername) - 1 - strlen(fullusername));
			ast_copy_string(mailbox, fullusername, mailbox_size);
		}

		ast_debug(1, "Before find user for mailbox %s\n", mailbox);
		vmu = find_user(&vmus, context, mailbox);
		if (vmu && (vmu->password[0] == '\0' || (vmu->password[0] == '-' && vmu->password[1] == '\0'))) {
			/* saved password is blank, so don't bother asking */
			password[0] = '\0';
		} else {
			if (ast_streamfile(chan, vm_password, chan->language)) {
				ast_log(LOG_WARNING, "Unable to stream password file\n");
				return -1;
			}
			if (ast_readstring(chan, password, sizeof(password) - 1, 2000, 10000, "#") < 0) {
				ast_log(LOG_WARNING, "Unable to read password\n");
				return -1;
			}
		}

		if (vmu) {
			passptr = vmu->password;
			if (passptr[0] == '-') passptr++;
		}
		if (vmu && !strcmp(passptr, password))
			valid++;
		else {
			ast_verb(3, "Incorrect password '%s' for user '%s' (context = %s)\n", password, mailbox, context ? context : "default");
			if (!ast_strlen_zero(prefix))
				mailbox[0] = '\0';
		}
		logretries++;
		if (!valid) {
			if (skipuser || logretries >= maxlogins) {
				if (ast_streamfile(chan, "vm-incorrect", chan->language)) {
					ast_log(LOG_WARNING, "Unable to stream incorrect message\n");
					return -1;
				}
			} else {
				if (useadsi)
					adsi_login(chan);
				if (ast_streamfile(chan, "vm-incorrect-mailbox", chan->language)) {
					ast_log(LOG_WARNING, "Unable to stream incorrect mailbox message\n");
					return -1;
				}
			}
			if (ast_waitstream(chan, ""))	/* Channel is hung up */
				return -1;
		}
	}
	if (!valid && (logretries >= maxlogins)) {
		ast_stopstream(chan);
		ast_play_and_wait(chan, "vm-goodbye");
		return -1;
	}
	if (vmu && !skipuser) {
		memcpy(res_vmu, vmu, sizeof(struct ast_vm_user));
	}
	return 0;
}

static int vm_execmain(struct ast_channel *chan, void *data)
{
	/* XXX This is, admittedly, some pretty horrendous code.  For some
	   reason it just seemed a lot easier to do with GOTO's.  I feel
	   like I'm back in my GWBASIC days. XXX */
	int res = -1;
	int cmd = 0;
	int valid = 0;
	char prefixstr[80] = "";
	char ext_context[256] = "";
	int box;
	int useadsi = 0;
	int skipuser = 0;
	struct vm_state vms;
	struct ast_vm_user *vmu = NULL, vmus;
	char *context = NULL;
	int silentexit = 0;
	struct ast_flags flags = { 0 };
	signed char record_gain = 0;
	int play_auto = 0;
	int play_folder = 0;
#ifdef IMAP_STORAGE
	int deleted = 0;
#endif

	/* Add the vm_state to the active list and keep it active */
	memset(&vms, 0, sizeof(vms));
	vms.lastmsg = -1;

	memset(&vmus, 0, sizeof(vmus));

	if (chan->_state != AST_STATE_UP) {
		ast_debug(1, "Before ast_answer\n");
		ast_answer(chan);
	}

	if (!ast_strlen_zero(data)) {
		char *opts[OPT_ARG_ARRAY_SIZE];
		char *parse;
		AST_DECLARE_APP_ARGS(args,
			AST_APP_ARG(argv0);
			AST_APP_ARG(argv1);
		);

		parse = ast_strdupa(data);

		AST_STANDARD_APP_ARGS(args, parse);

		if (args.argc == 2) {
			if (ast_app_parse_options(vm_app_options, &flags, opts, args.argv1))
				return -1;
			if (ast_test_flag(&flags, OPT_RECORDGAIN)) {
				int gain;
				if (!ast_strlen_zero(opts[OPT_ARG_RECORDGAIN])) {
					if (sscanf(opts[OPT_ARG_RECORDGAIN], "%d", &gain) != 1) {
						ast_log(LOG_WARNING, "Invalid value '%s' provided for record gain option\n", opts[OPT_ARG_RECORDGAIN]);
						return -1;
					} else {
						record_gain = (signed char) gain;
					}
				} else {
					ast_log(LOG_WARNING, "Invalid Gain level set with option g\n");
				}
			}
			if (ast_test_flag(&flags, OPT_AUTOPLAY) ) {
				play_auto = 1;
				if (opts[OPT_ARG_PLAYFOLDER]) {
					if (sscanf(opts[OPT_ARG_PLAYFOLDER], "%d", &play_folder) != 1) {
						ast_log(LOG_WARNING, "Invalid value '%s' provided for folder autoplay option\n", opts[OPT_ARG_PLAYFOLDER]);
					}
				} else {
					ast_log(LOG_WARNING, "Invalid folder set with option a\n");
				}	
				if (play_folder > 9 || play_folder < 0) {
					ast_log(LOG_WARNING, "Invalid value '%d' provided for folder autoplay option\n", play_folder);
					play_folder = 0;
				}
			}
		} else {
			/* old style options parsing */
			while (*(args.argv0)) {
				if (*(args.argv0) == 's')
					ast_set_flag(&flags, OPT_SILENT);
				else if (*(args.argv0) == 'p')
					ast_set_flag(&flags, OPT_PREPEND_MAILBOX);
				else 
					break;
				(args.argv0)++;
			}

		}

		valid = ast_test_flag(&flags, OPT_SILENT);

		if ((context = strchr(args.argv0, '@')))
			*context++ = '\0';

		if (ast_test_flag(&flags, OPT_PREPEND_MAILBOX))
			ast_copy_string(prefixstr, args.argv0, sizeof(prefixstr));
		else
			ast_copy_string(vms.username, args.argv0, sizeof(vms.username));

		if (!ast_strlen_zero(vms.username) && (vmu = find_user(&vmus, context, vms.username)))
			skipuser++;
		else
			valid = 0;
	}

	if (!valid)
		res = vm_authenticate(chan, vms.username, sizeof(vms.username), &vmus, context, prefixstr, skipuser, maxlogins, 0);

	ast_debug(1, "After vm_authenticate\n");
	if (!res) {
		valid = 1;
		if (!skipuser)
			vmu = &vmus;
	} else {
		res = 0;
	}

	/* If ADSI is supported, setup login screen */
	adsi_begin(chan, &useadsi);

#ifdef IMAP_STORAGE
	vms.interactive = 1;
	vms.updated = 1;
	vmstate_insert(&vms);
	init_vm_state(&vms);
#endif
	if (!valid)
		goto out;

	if (!(vms.deleted = ast_calloc(vmu->maxmsg, sizeof(int)))) {
		ast_log(LOG_ERROR, "Could not allocate memory for deleted message storage!\n");
		cmd = ast_play_and_wait(chan, "an-error-has-occured");
		return -1;
	}
	if (!(vms.heard = ast_calloc(vmu->maxmsg, sizeof(int)))) {
		ast_log(LOG_ERROR, "Could not allocate memory for heard message storage!\n");
		cmd = ast_play_and_wait(chan, "an-error-has-occured");
		return -1;
	}
	
	/* Set language from config to override channel language */
	if (!ast_strlen_zero(vmu->language))
		ast_string_field_set(chan, language, vmu->language);
	/* Retrieve old and new message counts */
	ast_debug(1, "Before open_mailbox\n");
	res = open_mailbox(&vms, vmu, OLD_FOLDER);
	if (res == ERROR_LOCK_PATH)
		goto out;
	vms.oldmessages = vms.lastmsg + 1;
	ast_debug(1, "Number of old messages: %d\n", vms.oldmessages);
	/* Start in INBOX */
	res = open_mailbox(&vms, vmu, NEW_FOLDER);
	if (res == ERROR_LOCK_PATH)
		goto out;
	vms.newmessages = vms.lastmsg + 1;
	ast_debug(1, "Number of new messages: %d\n", vms.newmessages);
		
	/* Select proper mailbox FIRST!! */
	if (play_auto) {
		res = open_mailbox(&vms, vmu, play_folder);
		if (res == ERROR_LOCK_PATH)
			goto out;

		/* If there are no new messages, inform the user and hangup */
		if (vms.lastmsg == -1) {
			cmd = vm_browse_messages(chan, &vms, vmu);
			res = 0;
			goto out;
		}
	} else {
		if (!vms.newmessages && vms.oldmessages) {
			/* If we only have old messages start here */
			res = open_mailbox(&vms, vmu, OLD_FOLDER);
			play_folder = 1;
			if (res == ERROR_LOCK_PATH)
				goto out;
		}
	}

	if (useadsi)
		adsi_status(chan, &vms);
	res = 0;

	/* Check to see if this is a new user */
	if (!strcasecmp(vmu->mailbox, vmu->password) && 
		(ast_test_flag(vmu, VM_FORCENAME | VM_FORCEGREET))) {
		if (ast_play_and_wait(chan, "vm-newuser") == -1)
			ast_log(LOG_WARNING, "Couldn't stream new user file\n");
		cmd = vm_newuser(chan, vmu, &vms, vmfmts, record_gain);
		if ((cmd == 't') || (cmd == '#')) {
			/* Timeout */
			res = 0;
			goto out;
		} else if (cmd < 0) {
			/* Hangup */
			res = -1;
			goto out;
		}
	}
#ifdef IMAP_STORAGE
		ast_debug(3, "Checking quotas: comparing %u to %u\n", vms.quota_usage, vms.quota_limit);
		if (vms.quota_limit && vms.quota_usage >= vms.quota_limit) {
			ast_debug(1, "*** QUOTA EXCEEDED!!\n");
			cmd = ast_play_and_wait(chan, "vm-mailboxfull");
		}
		ast_debug(3, "Checking quotas: User has %d messages and limit is %d.\n", (vms.newmessages + vms.oldmessages), vmu->maxmsg);
		if ((vms.newmessages + vms.oldmessages) >= vmu->maxmsg) {
			ast_log(LOG_WARNING, "No more messages possible.  User has %d messages and limit is %d.\n", (vms.newmessages + vms.oldmessages), vmu->maxmsg);
			cmd = ast_play_and_wait(chan, "vm-mailboxfull");
		}
#endif
	if (play_auto) {
		cmd = '1';
	} else {
		cmd = vm_intro(chan, vmu, &vms);
	}

	vms.repeats = 0;
	vms.starting = 1;
	while ((cmd > -1) && (cmd != 't') && (cmd != '#')) {
		/* Run main menu */
		switch (cmd) {
		case '1':
			vms.curmsg = 0;
			/* Fall through */
		case '5':
			cmd = vm_browse_messages(chan, &vms, vmu);
			break;
		case '2': /* Change folders */
			if (useadsi)
				adsi_folders(chan, 0, "Change to folder...");
			cmd = get_folder2(chan, "vm-changeto", 0);
			if (cmd == '#') {
				cmd = 0;
			} else if (cmd > 0) {
				cmd = cmd - '0';
				res = close_mailbox(&vms, vmu);
				if (res == ERROR_LOCK_PATH)
					goto out;
				res = open_mailbox(&vms, vmu, cmd);
				if (res == ERROR_LOCK_PATH)
					goto out;
				play_folder = cmd;
				cmd = 0;
			}
			if (useadsi)
				adsi_status2(chan, &vms);
				
			if (!cmd)
				cmd = vm_play_folder_name(chan, vms.vmbox);

			vms.starting = 1;
			break;
		case '3': /* Advanced options */
			cmd = 0;
			vms.repeats = 0;
			while ((cmd > -1) && (cmd != 't') && (cmd != '#')) {
				switch (cmd) {
				case '1': /* Reply */
					if (vms.lastmsg > -1 && !vms.starting) {
						cmd = advanced_options(chan, vmu, &vms, vms.curmsg, 1, record_gain);
						if (cmd == ERROR_LOCK_PATH) {
							res = cmd;
							goto out;
						}
					} else
						cmd = ast_play_and_wait(chan, "vm-sorry");
					cmd = 't';
					break;
				case '2': /* Callback */
					if (!vms.starting)
						ast_verb(3, "Callback Requested\n");
					if (!ast_strlen_zero(vmu->callback) && vms.lastmsg > -1 && !vms.starting) {
						cmd = advanced_options(chan, vmu, &vms, vms.curmsg, 2, record_gain);
						if (cmd == 9) {
							silentexit = 1;
							goto out;
						} else if (cmd == ERROR_LOCK_PATH) {
							res = cmd;
							goto out;
						}
					} else 
						cmd = ast_play_and_wait(chan, "vm-sorry");
					cmd = 't';
					break;
				case '3': /* Envelope */
					if (vms.lastmsg > -1 && !vms.starting) {
						cmd = advanced_options(chan, vmu, &vms, vms.curmsg, 3, record_gain);
						if (cmd == ERROR_LOCK_PATH) {
							res = cmd;
							goto out;
						}
					} else
						cmd = ast_play_and_wait(chan, "vm-sorry");
					cmd = 't';
					break;
				case '4': /* Dialout */
					if (!ast_strlen_zero(vmu->dialout)) {
						cmd = dialout(chan, vmu, NULL, vmu->dialout);
						if (cmd == 9) {
							silentexit = 1;
							goto out;
						}
					} else 
						cmd = ast_play_and_wait(chan, "vm-sorry");
					cmd = 't';
					break;

				case '5': /* Leave VoiceMail */
					if (ast_test_flag(vmu, VM_SVMAIL)) {
						cmd = forward_message(chan, context, &vms, vmu, vmfmts, 1, record_gain);
						if (cmd == ERROR_LOCK_PATH) {
							res = cmd;
							ast_log(LOG_WARNING, "forward_message failed to lock path.\n");
							goto out;
						}
					} else
						cmd = ast_play_and_wait(chan, "vm-sorry");
					cmd = 't';
					break;
					
				case '*': /* Return to main menu */
					cmd = 't';
					break;

				default:
					cmd = 0;
					if (!vms.starting) {
						cmd = ast_play_and_wait(chan, "vm-toreply");
					}
					if (!ast_strlen_zero(vmu->callback) && !vms.starting && !cmd) {
						cmd = ast_play_and_wait(chan, "vm-tocallback");
					}
					if (!cmd && !vms.starting) {
						cmd = ast_play_and_wait(chan, "vm-tohearenv");
					}
					if (!ast_strlen_zero(vmu->dialout) && !cmd) {
						cmd = ast_play_and_wait(chan, "vm-tomakecall");
					}
					if (ast_test_flag(vmu, VM_SVMAIL) && !cmd)
						cmd = ast_play_and_wait(chan, "vm-leavemsg");
					if (!cmd)
						cmd = ast_play_and_wait(chan, "vm-starmain");
					if (!cmd)
						cmd = ast_waitfordigit(chan, 6000);
					if (!cmd)
						vms.repeats++;
					if (vms.repeats > 3)
						cmd = 't';
				}
			}
			if (cmd == 't') {
				cmd = 0;
				vms.repeats = 0;
			}
			break;
		case '4':
			if (vms.curmsg > 0) {
				vms.curmsg--;
				cmd = play_message(chan, vmu, &vms);
			} else {
				cmd = ast_play_and_wait(chan, "vm-nomore");
			}
			break;
		case '6':
			if (vms.curmsg < vms.lastmsg) {
				vms.curmsg++;
				cmd = play_message(chan, vmu, &vms);
			} else {
				cmd = ast_play_and_wait(chan, "vm-nomore");
			}
			break;
		case '7':
			if (vms.curmsg >= 0 && vms.curmsg <= vms.lastmsg) {
				vms.deleted[vms.curmsg] = !vms.deleted[vms.curmsg];
				if (useadsi)
					adsi_delete(chan, &vms);
				if (vms.deleted[vms.curmsg]) {
					if (play_folder == 0)
						vms.newmessages--;
					else if (play_folder == 1)
						vms.oldmessages--;
					cmd = ast_play_and_wait(chan, "vm-deleted");
				} else {
					if (play_folder == 0)
						vms.newmessages++;
					else if (play_folder == 1)
						vms.oldmessages++;
					cmd = ast_play_and_wait(chan, "vm-undeleted");
				}
				if (ast_test_flag((&globalflags), VM_SKIPAFTERCMD)) {
					if (vms.curmsg < vms.lastmsg) {
						vms.curmsg++;
						cmd = play_message(chan, vmu, &vms);
					} else {
						cmd = ast_play_and_wait(chan, "vm-nomore");
					}
				}
			} else /* Delete not valid if we haven't selected a message */
				cmd = 0;
#ifdef IMAP_STORAGE
			deleted = 1;
#endif
			break;
	
		case '8':
			if (vms.lastmsg > -1) {
				cmd = forward_message(chan, context, &vms, vmu, vmfmts, 0, record_gain);
				if (cmd == ERROR_LOCK_PATH) {
					res = cmd;
					goto out;
				}
			} else
				cmd = ast_play_and_wait(chan, "vm-nomore");
			break;
		case '9':
			if (vms.curmsg < 0 || vms.curmsg > vms.lastmsg) {
				/* No message selected */
				cmd = 0;
				break;
			}
			if (useadsi)
				adsi_folders(chan, 1, "Save to folder...");
			cmd = get_folder2(chan, "vm-savefolder", 1);
			box = 0;	/* Shut up compiler */
			if (cmd == '#') {
				cmd = 0;
				break;
			} else if (cmd > 0) {
				box = cmd = cmd - '0';
				cmd = save_to_folder(vmu, &vms, vms.curmsg, cmd);
				if (cmd == ERROR_LOCK_PATH) {
					res = cmd;
					goto out;
#ifndef IMAP_STORAGE
				} else if (!cmd) {
					vms.deleted[vms.curmsg] = 1;
#endif
				} else {
					vms.deleted[vms.curmsg] = 0;
					vms.heard[vms.curmsg] = 0;
				}
			}
			make_file(vms.fn, sizeof(vms.fn), vms.curdir, vms.curmsg);
			if (useadsi)
				adsi_message(chan, &vms);
			snprintf(vms.fn, sizeof(vms.fn), "vm-%s", mbox(box));
			if (!cmd) {
				cmd = ast_play_and_wait(chan, "vm-message");
				if (!cmd) 
					cmd = say_and_wait(chan, vms.curmsg + 1, chan->language);
				if (!cmd)
					cmd = ast_play_and_wait(chan, "vm-savedto");
				if (!cmd)
					cmd = vm_play_folder_name(chan, vms.fn);
			} else {
				cmd = ast_play_and_wait(chan, "vm-mailboxfull");
			}
			if (ast_test_flag((&globalflags), VM_SKIPAFTERCMD)) {
				if (vms.curmsg < vms.lastmsg) {
					vms.curmsg++;
					cmd = play_message(chan, vmu, &vms);
				} else {
					cmd = ast_play_and_wait(chan, "vm-nomore");
				}
			}
			break;
		case '*':
			if (!vms.starting) {
				cmd = ast_play_and_wait(chan, "vm-onefor");
				if (!cmd)
					cmd = vm_play_folder_name(chan, vms.vmbox);
				if (!cmd)
					cmd = ast_play_and_wait(chan, "vm-opts");
				if (!cmd)
					cmd = vm_instructions(chan, &vms, 1);
			} else
				cmd = 0;
			break;
		case '0':
			cmd = vm_options(chan, vmu, &vms, vmfmts, record_gain);
			if (useadsi)
				adsi_status(chan, &vms);
			break;
		default:	/* Nothing */
			cmd = vm_instructions(chan, &vms, 0);
			break;
		}
	}
	if ((cmd == 't') || (cmd == '#')) {
		/* Timeout */
		res = 0;
	} else {
		/* Hangup */
		res = -1;
	}

out:
	if (res > -1) {
		ast_stopstream(chan);
		adsi_goodbye(chan);
		if (valid) {
			if (silentexit)
				res = ast_play_and_wait(chan, "vm-dialout");
			else 
				res = ast_play_and_wait(chan, "vm-goodbye");
			if (res > 0)
				res = 0;
		}
		if (useadsi)
			ast_adsi_unload_session(chan);
	}
	if (vmu)
		close_mailbox(&vms, vmu);
	if (valid) {
		int new = 0, old = 0;
		snprintf(ext_context, sizeof(ext_context), "%s@%s", vms.username, vmu->context);
		manager_event(EVENT_FLAG_CALL, "MessageWaiting", "Mailbox: %s\r\nWaiting: %d\r\n", ext_context, has_voicemail(ext_context, NULL));
		run_externnotify(vmu->context, vmu->mailbox);
		ast_app_inboxcount(ext_context, &new, &old);
		queue_mwi_event(ext_context, new, old);
	}
#ifdef IMAP_STORAGE
	/* expunge message - use UID Expunge if supported on IMAP server*/
	ast_debug(3, "*** Checking if we can expunge, deleted set to %d, expungeonhangup set to %d\n", deleted, expungeonhangup);
	if (vmu && deleted == 1 && expungeonhangup == 1) {
#ifdef HAVE_IMAP_TK2006
		if (LEVELUIDPLUS (vms.mailstream)) {
			mail_expunge_full(vms.mailstream, NIL, EX_UID);
		} else 
#endif
			mail_expunge(vms.mailstream);
	}
	/*  before we delete the state, we should copy pertinent info
	 *  back to the persistent model */
	vmstate_delete(&vms);
#endif
	if (vmu)
		free_user(vmu);
	if (vms.deleted)
		ast_free(vms.deleted);
	if (vms.heard)
		ast_free(vms.heard);

	return res;
}

static int vm_exec(struct ast_channel *chan, void *data)
{
	int res = 0;
	char *tmp;
	struct leave_vm_options leave_options;
	struct ast_flags flags = { 0 };
	char *opts[OPT_ARG_ARRAY_SIZE];
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(argv0);
		AST_APP_ARG(argv1);
	);
	
	memset(&leave_options, 0, sizeof(leave_options));

	if (chan->_state != AST_STATE_UP)
		ast_answer(chan);

	if (!ast_strlen_zero(data)) {
		tmp = ast_strdupa(data);
		AST_STANDARD_APP_ARGS(args, tmp);
		if (args.argc == 2) {
			if (ast_app_parse_options(vm_app_options, &flags, opts, args.argv1))
				return -1;
			ast_copy_flags(&leave_options, &flags, OPT_SILENT | OPT_BUSY_GREETING | OPT_UNAVAIL_GREETING | OPT_DTMFEXIT);
			if (ast_test_flag(&flags, OPT_RECORDGAIN)) {
				int gain;

				if (sscanf(opts[OPT_ARG_RECORDGAIN], "%d", &gain) != 1) {
					ast_log(LOG_WARNING, "Invalid value '%s' provided for record gain option\n", opts[OPT_ARG_RECORDGAIN]);
					return -1;
				} else {
					leave_options.record_gain = (signed char) gain;
				}
			}
			if (ast_test_flag(&flags, OPT_DTMFEXIT)) {
				if (!ast_strlen_zero(opts[OPT_ARG_DTMFEXIT]))
					leave_options.exitcontext = opts[OPT_ARG_DTMFEXIT];
			}
		}
	} else {
		char tmp[256];
		res = ast_app_getdata(chan, "vm-whichbox", tmp, sizeof(tmp) - 1, 0);
		if (res < 0)
			return res;
		if (ast_strlen_zero(tmp))
			return 0;
		args.argv0 = ast_strdupa(tmp);
	}

	res = leave_voicemail(chan, args.argv0, &leave_options);

	if (res == ERROR_LOCK_PATH) {
		ast_log(LOG_ERROR, "Could not leave voicemail. The path is already locked.\n");
		pbx_builtin_setvar_helper(chan, "VMSTATUS", "FAILED");
		res = 0;
	}

	return res;
}

static struct ast_vm_user *find_or_create(const char *context, const char *mbox)
{
	struct ast_vm_user *vmu;

	AST_LIST_TRAVERSE(&users, vmu, list) {
		if (ast_test_flag((&globalflags), VM_SEARCH) && !strcasecmp(mbox, vmu->mailbox))
			break;
		if (context && (!strcasecmp(context, vmu->context)) && (!strcasecmp(mbox, vmu->mailbox)))
			break;
	}

	if (vmu)
		return vmu;
	
	if (!(vmu = ast_calloc(1, sizeof(*vmu))))
		return NULL;
	
	ast_copy_string(vmu->context, context, sizeof(vmu->context));
	ast_copy_string(vmu->mailbox, mbox, sizeof(vmu->mailbox));

	AST_LIST_INSERT_TAIL(&users, vmu, list);
	
	return vmu;
}

static int append_mailbox(const char *context, const char *mbox, const char *data)
{
	/* Assumes lock is already held */
	char *tmp;
	char *stringp;
	char *s;
	struct ast_vm_user *vmu;
	char *mailbox_full;
	int new = 0, old = 0;

	tmp = ast_strdupa(data);

	if (!(vmu = find_or_create(context, mbox)))
		return -1;
	
	populate_defaults(vmu);

	stringp = tmp;
	if ((s = strsep(&stringp, ","))) 
		ast_copy_string(vmu->password, s, sizeof(vmu->password));
	if (stringp && (s = strsep(&stringp, ","))) 
		ast_copy_string(vmu->fullname, s, sizeof(vmu->fullname));
	if (stringp && (s = strsep(&stringp, ","))) 
		ast_copy_string(vmu->email, s, sizeof(vmu->email));
	if (stringp && (s = strsep(&stringp, ","))) 
		ast_copy_string(vmu->pager, s, sizeof(vmu->pager));
	if (stringp && (s = strsep(&stringp, ","))) 
		apply_options(vmu, s);

	mailbox_full = alloca(strlen(mbox) + strlen(context) + 1);
	strcpy(mailbox_full, mbox);
	strcat(mailbox_full, "@");
	strcat(mailbox_full, context);

	inboxcount(mailbox_full, &new, &old);
	queue_mwi_event(mailbox_full, new, old);

	return 0;
}

static int vm_box_exists(struct ast_channel *chan, void *data) 
{
	struct ast_vm_user svm;
	char *context, *box;
	AST_DECLARE_APP_ARGS(args,
		AST_APP_ARG(mbox);
		AST_APP_ARG(options);
	);
	static int dep_warning = 0;

	if (ast_strlen_zero(data)) {
		ast_log(LOG_ERROR, "MailboxExists requires an argument: (vmbox[@context][|options])\n");
		return -1;
	}

	if (!dep_warning) {
		dep_warning = 1;
		ast_log(LOG_WARNING, "MailboxExists is deprecated.  Please use ${MAILBOX_EXISTS(%s)} instead.\n", (char *)data);
	}

	box = ast_strdupa(data);

	AST_STANDARD_APP_ARGS(args, box);

	if (args.options) {
	}

	if ((context = strchr(args.mbox, '@'))) {
		*context = '\0';
		context++;
	}

	if (find_user(&svm, context, args.mbox)) {
		pbx_builtin_setvar_helper(chan, "VMBOXEXISTSSTATUS", "SUCCESS");
	} else
		pbx_builtin_setvar_helper(chan, "VMBOXEXISTSSTATUS", "FAILED");

	return 0;
}

static int acf_mailbox_exists(struct ast_channel *chan, const char *cmd, char *args, char *buf, size_t len)
{
	struct ast_vm_user svm;
	AST_DECLARE_APP_ARGS(arg,
		AST_APP_ARG(mbox);
		AST_APP_ARG(context);
	);

	AST_NONSTANDARD_APP_ARGS(arg, args, '@');

	ast_copy_string(buf, find_user(&svm, ast_strlen_zero(arg.context) ? "default" : arg.context, arg.mbox) ? "1" : "0", len);
	return 0;
}

static struct ast_custom_function mailbox_exists_acf = {
	.name = "MAILBOX_EXISTS",
	.synopsis = "Tell if a mailbox is configured",
	.desc =
"Returns a boolean of whether the corresponding mailbox exists.  If context\n"
"is not specified, defaults to the \"default\" context.\n",
	.syntax = "MAILBOX_EXISTS(<vmbox>[@<context>])",
	.read = acf_mailbox_exists,
};

static int vmauthenticate(struct ast_channel *chan, void *data)
{
	char *s = data, *user = NULL, *context = NULL, mailbox[AST_MAX_EXTENSION] = "";
	struct ast_vm_user vmus;
	char *options = NULL;
	int silent = 0, skipuser = 0;
	int res = -1;
	
	if (s) {
		s = ast_strdupa(s);
		user = strsep(&s, ",");
		options = strsep(&s, ",");
		if (user) {
			s = user;
			user = strsep(&s, "@");
			context = strsep(&s, "");
			if (!ast_strlen_zero(user))
				skipuser++;
			ast_copy_string(mailbox, user, sizeof(mailbox));
		}
	}

	if (options) {
		silent = (strchr(options, 's')) != NULL;
	}

	if (!vm_authenticate(chan, mailbox, sizeof(mailbox), &vmus, context, NULL, skipuser, 3, silent)) {
		pbx_builtin_setvar_helper(chan, "AUTH_MAILBOX", mailbox);
		pbx_builtin_setvar_helper(chan, "AUTH_CONTEXT", vmus.context);
		ast_play_and_wait(chan, "auth-thankyou");
		res = 0;
	}

	return res;
}

static char *show_users_realtime(int fd, const char *context)
{
	struct ast_config *cfg;
	const char *cat = NULL;

	if (!(cfg = ast_load_realtime_multientry("voicemail", 
		"context", context, NULL))) {
		return CLI_FAILURE;
	}

	ast_cli(fd,
		"\n"
		"=============================================================\n"
		"=== Configured Voicemail Users ==============================\n"
		"=============================================================\n"
		"===\n");

	while ((cat = ast_category_browse(cfg, cat))) {
		struct ast_variable *var = NULL;
		ast_cli(fd,
			"=== Mailbox ...\n"
			"===\n");
		for (var = ast_variable_browse(cfg, cat); var; var = var->next)
			ast_cli(fd, "=== ==> %s: %s\n", var->name, var->value);
		ast_cli(fd,
			"===\n"
			"=== ---------------------------------------------------------\n"
			"===\n");
	}

	ast_cli(fd,
		"=============================================================\n"
		"\n");

	return CLI_SUCCESS;
}

static char *complete_voicemail_show_users(const char *line, const char *word, int pos, int state)
{
	int which = 0;
	int wordlen;
	struct ast_vm_user *vmu;
	const char *context = "";

	/* 0 - show; 1 - voicemail; 2 - users; 3 - for; 4 - <context> */
	if (pos > 4)
		return NULL;
	if (pos == 3)
		return (state == 0) ? ast_strdup("for") : NULL;
	wordlen = strlen(word);
	AST_LIST_TRAVERSE(&users, vmu, list) {
		if (!strncasecmp(word, vmu->context, wordlen)) {
			if (context && strcmp(context, vmu->context) && ++which > state)
				return ast_strdup(vmu->context);
			/* ignore repeated contexts ? */
			context = vmu->context;
		}
	}
	return NULL;
}

/*! \brief Show a list of voicemail users in the CLI */
static char *handle_voicemail_show_users(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct ast_vm_user *vmu;
#define HVSU_OUTPUT_FORMAT "%-10s %-5s %-25s %-10s %6s\n"
	const char *context = NULL;
	int users_counter = 0;

	switch (cmd) {
	case CLI_INIT:
		e->command = "voicemail show users";
		e->usage =
			"Usage: voicemail show users [for <context>]\n"
			"       Lists all mailboxes currently set up\n";
		return NULL;
	case CLI_GENERATE:
		return complete_voicemail_show_users(a->line, a->word, a->pos, a->n);
	}	

	if ((a->argc < 3) || (a->argc > 5) || (a->argc == 4))
		return CLI_SHOWUSAGE;
	if (a->argc == 5) {
		if (strcmp(a->argv[3], "for"))
			return CLI_SHOWUSAGE;
		context = a->argv[4];
	}

	if (ast_check_realtime("voicemail")) {
		if (!context) {
			ast_cli(a->fd, "You must specify a specific context to show users from realtime!\n");
			return CLI_SHOWUSAGE;
		}
		return show_users_realtime(a->fd, context);
	}

	AST_LIST_LOCK(&users);
	if (AST_LIST_EMPTY(&users)) {
		ast_cli(a->fd, "There are no voicemail users currently defined\n");
		AST_LIST_UNLOCK(&users);
		return CLI_FAILURE;
	}
	if (a->argc == 3)
		ast_cli(a->fd, HVSU_OUTPUT_FORMAT, "Context", "Mbox", "User", "Zone", "NewMsg");
	else {
		int count = 0;
		AST_LIST_TRAVERSE(&users, vmu, list) {
			if (!strcmp(context, vmu->context))
				count++;
		}
		if (count) {
			ast_cli(a->fd, HVSU_OUTPUT_FORMAT, "Context", "Mbox", "User", "Zone", "NewMsg");
		} else {
			ast_cli(a->fd, "No such voicemail context \"%s\"\n", context);
			AST_LIST_UNLOCK(&users);
			return CLI_FAILURE;
		}
	}
	AST_LIST_TRAVERSE(&users, vmu, list) {
		int newmsgs = 0, oldmsgs = 0;
		char count[12], tmp[256] = "";

		if ((a->argc == 3) || ((a->argc == 5) && !strcmp(context, vmu->context))) {
			snprintf(tmp, sizeof(tmp), "%s@%s", vmu->mailbox, ast_strlen_zero(vmu->context) ? "default" : vmu->context);
			inboxcount(tmp, &newmsgs, &oldmsgs);
			snprintf(count, sizeof(count), "%d", newmsgs);
			ast_cli(a->fd, HVSU_OUTPUT_FORMAT, vmu->context, vmu->mailbox, vmu->fullname, vmu->zonetag, count);
			users_counter++;
		}
	}
	AST_LIST_UNLOCK(&users);
	ast_cli(a->fd, "%d voicemail users configured.\n", users_counter);
	return CLI_SUCCESS;
}

/*! \brief Show a list of voicemail zones in the CLI */
static char *handle_voicemail_show_zones(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	struct vm_zone *zone;
#define HVSZ_OUTPUT_FORMAT "%-15s %-20s %-45s\n"
	char *res = CLI_SUCCESS;

	switch (cmd) {
	case CLI_INIT:
		e->command = "voicemail show zones";
		e->usage =
			"Usage: voicemail show zones\n"
			"       Lists zone message formats\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	AST_LIST_LOCK(&zones);
	if (!AST_LIST_EMPTY(&zones)) {
		ast_cli(a->fd, HVSZ_OUTPUT_FORMAT, "Zone", "Timezone", "Message Format");
		AST_LIST_TRAVERSE(&zones, zone, list) {
			ast_cli(a->fd, HVSZ_OUTPUT_FORMAT, zone->name, zone->timezone, zone->msg_format);
		}
	} else {
		ast_cli(a->fd, "There are no voicemail zones currently defined\n");
		res = CLI_FAILURE;
	}
	AST_LIST_UNLOCK(&zones);

	return res;
}

/*! \brief Reload voicemail configuration from the CLI */
static char *handle_voicemail_reload(struct ast_cli_entry *e, int cmd, struct ast_cli_args *a)
{
	switch (cmd) {
	case CLI_INIT:
		e->command = "voicemail reload";
		e->usage =
			"Usage: voicemail reload\n"
			"       Reload voicemail configuration\n";
		return NULL;
	case CLI_GENERATE:
		return NULL;
	}

	if (a->argc != e->args)
		return CLI_SHOWUSAGE;

	ast_cli(a->fd, "Reloading voicemail configuration...\n");	
	load_config(1);
	
	return CLI_SUCCESS;
}

static struct ast_cli_entry cli_voicemail[] = {
	AST_CLI_DEFINE(handle_voicemail_show_users, "List defined voicemail boxes"),
	AST_CLI_DEFINE(handle_voicemail_show_zones, "List zone message formats"),
	AST_CLI_DEFINE(handle_voicemail_reload, "Reload voicemail configuration"),
};

static void poll_subscribed_mailboxes(void)
{
	struct mwi_sub *mwi_sub;

	AST_RWLIST_RDLOCK(&mwi_subs);
	AST_RWLIST_TRAVERSE(&mwi_subs, mwi_sub, entry) {
		int new = 0, old = 0;

		if (ast_strlen_zero(mwi_sub->mailbox))
			continue;

		inboxcount(mwi_sub->mailbox, &new, &old);

		if (new != mwi_sub->old_new || old != mwi_sub->old_old) {
			mwi_sub->old_new = new;
			mwi_sub->old_old = old;
			queue_mwi_event(mwi_sub->mailbox, new, old);
		}
	}
	AST_RWLIST_UNLOCK(&mwi_subs);
}

static void *mb_poll_thread(void *data)
{
	while (poll_thread_run) {
		struct timespec ts = { 0, };
		struct timeval tv;

		tv = ast_tvadd(ast_tvnow(), ast_samp2tv(poll_freq, 1));
		ts.tv_sec = tv.tv_sec;
		ts.tv_nsec = tv.tv_usec * 1000;

		ast_mutex_lock(&poll_lock);
		ast_cond_timedwait(&poll_cond, &poll_lock, &ts);
		ast_mutex_unlock(&poll_lock);

		if (!poll_thread_run)
			break;

		poll_subscribed_mailboxes();
	}

	return NULL;
}

static void mwi_sub_destroy(struct mwi_sub *mwi_sub)
{
	ast_free(mwi_sub);
}

static void mwi_unsub_event_cb(const struct ast_event *event, void *userdata)
{
	uint32_t uniqueid;
	struct mwi_sub *mwi_sub;

	if (ast_event_get_type(event) != AST_EVENT_UNSUB)
		return;

	if (ast_event_get_ie_uint(event, AST_EVENT_IE_EVENTTYPE) != AST_EVENT_MWI)
		return;

	uniqueid = ast_event_get_ie_uint(event, AST_EVENT_IE_UNIQUEID);

	AST_RWLIST_WRLOCK(&mwi_subs);
	AST_RWLIST_TRAVERSE_SAFE_BEGIN(&mwi_subs, mwi_sub, entry) {
		if (mwi_sub->uniqueid == uniqueid) {
			AST_LIST_REMOVE_CURRENT(entry);
			break;
		}
	}
	AST_RWLIST_TRAVERSE_SAFE_END
	AST_RWLIST_UNLOCK(&mwi_subs);

	if (mwi_sub)
		mwi_sub_destroy(mwi_sub);
}

static void mwi_sub_event_cb(const struct ast_event *event, void *userdata)
{
	const char *mailbox;
	const char *context;
	uint32_t uniqueid;
	unsigned int len;
	struct mwi_sub *mwi_sub;

	if (ast_event_get_type(event) != AST_EVENT_SUB)
		return;

	if (ast_event_get_ie_uint(event, AST_EVENT_IE_EVENTTYPE) != AST_EVENT_MWI)
		return;

	mailbox = ast_event_get_ie_str(event, AST_EVENT_IE_MAILBOX);
	context = ast_event_get_ie_str(event, AST_EVENT_IE_CONTEXT);
	uniqueid = ast_event_get_ie_uint(event, AST_EVENT_IE_UNIQUEID);

	len = sizeof(*mwi_sub);
	if (!ast_strlen_zero(mailbox))
		len += strlen(mailbox);

	if (!ast_strlen_zero(context))
		len += strlen(context) + 1; /* Allow for seperator */

	if (!(mwi_sub = ast_calloc(1, len)))
		return;

	mwi_sub->uniqueid = uniqueid;
	if (!ast_strlen_zero(mailbox))
		strcpy(mwi_sub->mailbox, mailbox);

	if (!ast_strlen_zero(context)) {
		strcat(mwi_sub->mailbox, "@");
		strcat(mwi_sub->mailbox, context);
	}

	AST_RWLIST_WRLOCK(&mwi_subs);
	AST_RWLIST_INSERT_TAIL(&mwi_subs, mwi_sub, entry);
	AST_RWLIST_UNLOCK(&mwi_subs);
}

static void start_poll_thread(void)
{
	pthread_attr_t attr;

	mwi_sub_sub = ast_event_subscribe(AST_EVENT_SUB, mwi_sub_event_cb, NULL,
		AST_EVENT_IE_EVENTTYPE, AST_EVENT_IE_PLTYPE_UINT, AST_EVENT_MWI,
		AST_EVENT_IE_END);

	mwi_unsub_sub = ast_event_subscribe(AST_EVENT_UNSUB, mwi_unsub_event_cb, NULL,
		AST_EVENT_IE_EVENTTYPE, AST_EVENT_IE_PLTYPE_UINT, AST_EVENT_MWI,
		AST_EVENT_IE_END);

	if (mwi_sub_sub)
		ast_event_report_subs(mwi_sub_sub);

	poll_thread_run = 1;

	pthread_attr_init(&attr);
	ast_pthread_create(&poll_thread, &attr, mb_poll_thread, NULL);
	pthread_attr_destroy(&attr);
}

static void stop_poll_thread(void)
{
	poll_thread_run = 0;

	if (mwi_sub_sub) {
		ast_event_unsubscribe(mwi_sub_sub);
		mwi_sub_sub = NULL;
	}

	if (mwi_unsub_sub) {
		ast_event_unsubscribe(mwi_unsub_sub);
		mwi_unsub_sub = NULL;
	}

	ast_mutex_lock(&poll_lock);
	ast_cond_signal(&poll_cond);
	ast_mutex_unlock(&poll_lock);

	pthread_join(poll_thread, NULL);

	poll_thread = AST_PTHREADT_NULL;
}

/*! \brief Manager list voicemail users command */
static int manager_list_voicemail_users(struct mansession *s, const struct message *m)
{
	struct ast_vm_user *vmu = NULL;
	const char *id = astman_get_header(m, "ActionID");
	char actionid[128] = "";

	if (!ast_strlen_zero(id))
		snprintf(actionid, sizeof(actionid), "ActionID: %s\r\n", id);

	AST_LIST_LOCK(&users);

	if (AST_LIST_EMPTY(&users)) {
		astman_send_ack(s, m, "There are no voicemail users currently defined.");
		AST_LIST_UNLOCK(&users);
		astman_append(s, "Event: VoicemailUserEntryComplete\r\n%s\r\n", actionid);
		return RESULT_SUCCESS;
	}
	
	astman_send_ack(s, m, "Voicemail user list will follow");
	
	AST_LIST_TRAVERSE(&users, vmu, list) {
		char dirname[256];

#ifdef IMAP_STORAGE
		int new, old;
		inboxcount (vmu->mailbox, &new, &old);
#endif
		
		make_dir(dirname, sizeof(dirname), vmu->context, vmu->mailbox, "INBOX");
		astman_append(s,
			"%s"
			"Event: VoicemailUserEntry\r\n"
			"VMContext: %s\r\n"
			"VoiceMailbox: %s\r\n"
			"Fullname: %s\r\n"
			"Email: %s\r\n"
			"Pager: %s\r\n"
			"ServerEmail: %s\r\n"
			"MailCommand: %s\r\n"
			"Language: %s\r\n"
			"TimeZone: %s\r\n"
			"Callback: %s\r\n"
			"Dialout: %s\r\n"
			"UniqueID: %s\r\n"
			"ExitContext: %s\r\n"
			"SayDurationMinimum: %d\r\n"
			"SayEnvelope: %s\r\n"
			"SayCID: %s\r\n"
			"AttachMessage: %s\r\n"
			"AttachmentFormat: %s\r\n"
			"DeleteMessage: %s\r\n"
			"VolumeGain: %.2f\r\n"
			"CanReview: %s\r\n"
			"CallOperator: %s\r\n"
			"MaxMessageCount: %d\r\n"
			"MaxMessageLength: %d\r\n"
			"NewMessageCount: %d\r\n"
#ifdef IMAP_STORAGE
			"OldMessageCount: %d\r\n"
			"IMAPUser: %s\r\n"
#endif
			"\r\n",
			actionid,
			vmu->context,
			vmu->mailbox,
			vmu->fullname,
			vmu->email,
			vmu->pager,
			vmu->serveremail,
			vmu->mailcmd,
			vmu->language,
			vmu->zonetag,
			vmu->callback,
			vmu->dialout,
			vmu->uniqueid,
			vmu->exit,
			vmu->saydurationm,
			ast_test_flag(vmu, VM_ENVELOPE) ? "Yes" : "No",
			ast_test_flag(vmu, VM_SAYCID) ? "Yes" : "No",
			ast_test_flag(vmu, VM_ATTACH) ? "Yes" : "No",
			vmu->attachfmt,
			ast_test_flag(vmu, VM_DELETE) ? "Yes" : "No",
			vmu->volgain,
			ast_test_flag(vmu, VM_REVIEW) ? "Yes" : "No",
			ast_test_flag(vmu, VM_OPERATOR) ? "Yes" : "No",
			vmu->maxmsg,
			vmu->maxsecs,
#ifdef IMAP_STORAGE
			new, old, vmu->imapuser
#else
			count_messages(vmu, dirname)
#endif
			);
	}		
	astman_append(s, "Event: VoicemailUserEntryComplete\r\n%s\r\n", actionid);

	AST_LIST_UNLOCK(&users);

	return RESULT_SUCCESS;
}

/*! \brief Free the users structure. */
static void free_vm_users(void) 
{
	struct ast_vm_user *cur;
	AST_LIST_LOCK(&users);
	while ((cur = AST_LIST_REMOVE_HEAD(&users, list))) {
		ast_set_flag(cur, VM_ALLOCED);
		free_user(cur);
	}
	AST_LIST_UNLOCK(&users);
}

/*! \brief Free the zones structure. */
static void free_vm_zones(void)
{
	struct vm_zone *zcur;
	AST_LIST_LOCK(&zones);
	while ((zcur = AST_LIST_REMOVE_HEAD(&zones, list)))
		free_zone(zcur);
	AST_LIST_UNLOCK(&zones);
}

static int load_config(int reload)
{
	struct ast_vm_user *cur;
	struct ast_config *cfg, *ucfg;
	char *cat;
	struct ast_variable *var;
	const char *val;
	char *q, *stringp;
	int x;
	int tmpadsi[4];
	struct ast_flags config_flags = { reload ? CONFIG_FLAG_FILEUNCHANGED : 0 };

	if ((cfg = ast_config_load(VOICEMAIL_CONFIG, config_flags)) == CONFIG_STATUS_FILEUNCHANGED) {
		if ((ucfg = ast_config_load("users.conf", config_flags)) == CONFIG_STATUS_FILEUNCHANGED)
			return 0;
		ast_clear_flag(&config_flags, CONFIG_FLAG_FILEUNCHANGED);
		cfg = ast_config_load(VOICEMAIL_CONFIG, config_flags);
	} else {
		ast_clear_flag(&config_flags, CONFIG_FLAG_FILEUNCHANGED);
		ucfg = ast_config_load("users.conf", config_flags);
	}
#ifdef IMAP_STORAGE
	ast_copy_string(imapparentfolder, "\0", sizeof(imapparentfolder));
#endif
	/* set audio control prompts */
	strcpy(listen_control_forward_key, DEFAULT_LISTEN_CONTROL_FORWARD_KEY);
	strcpy(listen_control_reverse_key, DEFAULT_LISTEN_CONTROL_REVERSE_KEY);
	strcpy(listen_control_pause_key, DEFAULT_LISTEN_CONTROL_PAUSE_KEY);
	strcpy(listen_control_restart_key, DEFAULT_LISTEN_CONTROL_RESTART_KEY);
	strcpy(listen_control_stop_key, DEFAULT_LISTEN_CONTROL_STOP_KEY);

	/* Free all the users structure */	
	free_vm_users();

	/* Free all the zones structure */
	free_vm_zones();

	AST_LIST_LOCK(&users);	

	memset(ext_pass_cmd, 0, sizeof(ext_pass_cmd));

	if (cfg) {
		/* General settings */

		if (!(val = ast_variable_retrieve(cfg, "general", "userscontext")))
			val = "default";
		ast_copy_string(userscontext, val, sizeof(userscontext));
		/* Attach voice message to mail message ? */
		if (!(val = ast_variable_retrieve(cfg, "general", "attach"))) 
			val = "yes";
		ast_set2_flag((&globalflags), ast_true(val), VM_ATTACH);	

		if (!(val = ast_variable_retrieve(cfg, "general", "searchcontexts")))
			val = "no";
		ast_set2_flag((&globalflags), ast_true(val), VM_SEARCH);

		volgain = 0.0;
		if ((val = ast_variable_retrieve(cfg, "general", "volgain")))
			sscanf(val, "%lf", &volgain);

#ifdef ODBC_STORAGE
		strcpy(odbc_database, "asterisk");
		if ((val = ast_variable_retrieve(cfg, "general", "odbcstorage"))) {
			ast_copy_string(odbc_database, val, sizeof(odbc_database));
		}
		strcpy(odbc_table, "voicemessages");
		if ((val = ast_variable_retrieve(cfg, "general", "odbctable"))) {
			ast_copy_string(odbc_table, val, sizeof(odbc_table));
		}
#endif		
		/* Mail command */
		strcpy(mailcmd, SENDMAIL);
		if ((val = ast_variable_retrieve(cfg, "general", "mailcmd")))
			ast_copy_string(mailcmd, val, sizeof(mailcmd)); /* User setting */

		maxsilence = 0;
		if ((val = ast_variable_retrieve(cfg, "general", "maxsilence"))) {
			maxsilence = atoi(val);
			if (maxsilence > 0)
				maxsilence *= 1000;
		}
		
		if (!(val = ast_variable_retrieve(cfg, "general", "maxmsg"))) {
			maxmsg = MAXMSG;
		} else {
			maxmsg = atoi(val);
			if (maxmsg <= 0) {
				ast_log(LOG_WARNING, "Invalid number of messages per folder '%s'. Using default value %i\n", val, MAXMSG);
				maxmsg = MAXMSG;
			} else if (maxmsg > MAXMSGLIMIT) {
				ast_log(LOG_WARNING, "Maximum number of messages per folder is %i. Cannot accept value '%s'\n", MAXMSGLIMIT, val);
				maxmsg = MAXMSGLIMIT;
			}
		}

		if (!(val = ast_variable_retrieve(cfg, "general", "backupdeleted"))) {
			maxdeletedmsg = 0;
		} else {
			if (sscanf(val, "%d", &x) == 1)
				maxdeletedmsg = x;
			else if (ast_true(val))
				maxdeletedmsg = MAXMSG;
			else
				maxdeletedmsg = 0;

			if (maxdeletedmsg < 0) {
				ast_log(LOG_WARNING, "Invalid number of deleted messages saved per mailbox '%s'. Using default value %i\n", val, MAXMSG);
				maxdeletedmsg = MAXMSG;
			} else if (maxdeletedmsg > MAXMSGLIMIT) {
				ast_log(LOG_WARNING, "Maximum number of deleted messages saved per mailbox is %i. Cannot accept value '%s'\n", MAXMSGLIMIT, val);
				maxdeletedmsg = MAXMSGLIMIT;
			}
		}

		/* Load date format config for voicemail mail */
		if ((val = ast_variable_retrieve(cfg, "general", "emaildateformat"))) {
			ast_copy_string(emaildateformat, val, sizeof(emaildateformat));
		}

		/* External password changing command */
		if ((val = ast_variable_retrieve(cfg, "general", "externpass"))) {
			ast_copy_string(ext_pass_cmd, val, sizeof(ext_pass_cmd));
			pwdchange = PWDCHANGE_EXTERNAL;
		} else if ((val = ast_variable_retrieve(cfg, "general", "externpassnotify"))) {
			ast_copy_string(ext_pass_cmd, val, sizeof(ext_pass_cmd));
			pwdchange = PWDCHANGE_EXTERNAL | PWDCHANGE_INTERNAL;
		}

#ifdef IMAP_STORAGE
		/* IMAP server address */
		if ((val = ast_variable_retrieve(cfg, "general", "imapserver"))) {
			ast_copy_string(imapserver, val, sizeof(imapserver));
		} else {
			ast_copy_string(imapserver, "localhost", sizeof(imapserver));
		}
		/* IMAP server port */
		if ((val = ast_variable_retrieve(cfg, "general", "imapport"))) {
			ast_copy_string(imapport, val, sizeof(imapport));
		} else {
			ast_copy_string(imapport, "143", sizeof(imapport));
		}
		/* IMAP server flags */
		if ((val = ast_variable_retrieve(cfg, "general", "imapflags"))) {
			ast_copy_string(imapflags, val, sizeof(imapflags));
		}
		/* IMAP server master username */
		if ((val = ast_variable_retrieve(cfg, "general", "authuser"))) {
			ast_copy_string(authuser, val, sizeof(authuser));
		}
		/* IMAP server master password */
		if ((val = ast_variable_retrieve(cfg, "general", "authpassword"))) {
			ast_copy_string(authpassword, val, sizeof(authpassword));
		}
		/* Expunge on exit */
		if ((val = ast_variable_retrieve(cfg, "general", "expungeonhangup"))) {
			if (ast_false(val))
				expungeonhangup = 0;
			else
				expungeonhangup = 1;
		} else {
			expungeonhangup = 1;
		}
		/* IMAP voicemail folder */
		if ((val = ast_variable_retrieve(cfg, "general", "imapfolder"))) {
			ast_copy_string(imapfolder, val, sizeof(imapfolder));
		} else {
			ast_copy_string(imapfolder, "INBOX", sizeof(imapfolder));
		}
		if ((val = ast_variable_retrieve(cfg, "general", "imapparentfolder"))) {
			ast_copy_string(imapparentfolder, val, sizeof(imapparentfolder));
		}
		if ((val = ast_variable_retrieve(cfg, "general", "imapgreetings"))) {
			imapgreetings = ast_true(val);
		} else {
			imapgreetings = 0;
		}
		if ((val = ast_variable_retrieve(cfg, "general", "greetingfolder"))) {
			ast_copy_string(greetingfolder, val, sizeof(greetingfolder));
		} else {
			ast_copy_string(greetingfolder, imapfolder, sizeof(greetingfolder));
		}

		/* There is some very unorthodox casting done here. This is due
		 * to the way c-client handles the argument passed in. It expects a 
		 * void pointer and casts the pointer directly to a long without
		 * first dereferencing it. */
		if ((val = ast_variable_retrieve(cfg, "general", "imapreadtimeout"))) {
			mail_parameters(NIL, SET_READTIMEOUT, (void *) (atol(val)));
		} else {
			mail_parameters(NIL, SET_READTIMEOUT, (void *) 60L);
		}

		if ((val = ast_variable_retrieve(cfg, "general", "imapwritetimeout"))) {
			mail_parameters(NIL, SET_WRITETIMEOUT, (void *) (atol(val)));
		} else {
			mail_parameters(NIL, SET_WRITETIMEOUT, (void *) 60L);
		}

		if ((val = ast_variable_retrieve(cfg, "general", "imapopentimeout"))) {
			mail_parameters(NIL, SET_OPENTIMEOUT, (void *) (atol(val)));
		} else {
			mail_parameters(NIL, SET_OPENTIMEOUT, (void *) 60L);
		}

		if ((val = ast_variable_retrieve(cfg, "general", "imapclosetimeout"))) {
			mail_parameters(NIL, SET_CLOSETIMEOUT, (void *) (atol(val)));
		} else {
			mail_parameters(NIL, SET_CLOSETIMEOUT, (void *) 60L);
		}

#endif
		/* External voicemail notify application */
		if ((val = ast_variable_retrieve(cfg, "general", "externnotify"))) {
			ast_copy_string(externnotify, val, sizeof(externnotify));
			ast_debug(1, "found externnotify: %s\n", externnotify);
		} else {
			externnotify[0] = '\0';
		}

		/* SMDI voicemail notification */
		if ((val = ast_variable_retrieve(cfg, "general", "smdienable")) && ast_true(val)) {
			ast_debug(1, "Enabled SMDI voicemail notification\n");
			if ((val = ast_variable_retrieve(cfg, "general", "smdiport"))) {
				smdi_iface = ast_smdi_interface_find(val);
			} else {
				ast_debug(1, "No SMDI interface set, trying default (/dev/ttyS0)\n");
				smdi_iface = ast_smdi_interface_find("/dev/ttyS0");
			}
			if (!smdi_iface) {
				ast_log(LOG_ERROR, "No valid SMDI interface specfied, disabling SMDI voicemail notification\n");
			}
		}

		/* Silence treshold */
		silencethreshold = 256;
		if ((val = ast_variable_retrieve(cfg, "general", "silencethreshold")))
			silencethreshold = atoi(val);
		
		if (!(val = ast_variable_retrieve(cfg, "general", "serveremail"))) 
			val = ASTERISK_USERNAME;
		ast_copy_string(serveremail, val, sizeof(serveremail));
		
		vmmaxsecs = 0;
		if ((val = ast_variable_retrieve(cfg, "general", "maxsecs"))) {
			if (sscanf(val, "%d", &x) == 1) {
				vmmaxsecs = x;
			} else {
				ast_log(LOG_WARNING, "Invalid max message time length\n");
			}
		} else if ((val = ast_variable_retrieve(cfg, "general", "maxmessage"))) {
			static int maxmessage_deprecate = 0;
			if (maxmessage_deprecate == 0) {
				maxmessage_deprecate = 1;
				ast_log(LOG_WARNING, "Setting 'maxmessage' has been deprecated in favor of 'maxsecs'.\n");
			}
			if (sscanf(val, "%d", &x) == 1) {
				vmmaxsecs = x;
			} else {
				ast_log(LOG_WARNING, "Invalid max message time length\n");
			}
		}

		vmminsecs = 0;
		if ((val = ast_variable_retrieve(cfg, "general", "minsecs"))) {
			if (sscanf(val, "%d", &x) == 1) {
				vmminsecs = x;
				if (maxsilence <= vmminsecs)
					ast_log(LOG_WARNING, "maxsilence should be less than minmessage or you may get empty messages\n");
			} else {
				ast_log(LOG_WARNING, "Invalid min message time length\n");
			}
		} else if ((val = ast_variable_retrieve(cfg, "general", "minmessage"))) {
			static int maxmessage_deprecate = 0;
			if (maxmessage_deprecate == 0) {
				maxmessage_deprecate = 1;
				ast_log(LOG_WARNING, "Setting 'minmessage' has been deprecated in favor of 'minsecs'.\n");
			}
			if (sscanf(val, "%d", &x) == 1) {
				vmminsecs = x;
				if (maxsilence <= vmminsecs)
					ast_log(LOG_WARNING, "maxsilence should be less than minmessage or you may get empty messages\n");
			} else {
				ast_log(LOG_WARNING, "Invalid min message time length\n");
			}
		}

		val = ast_variable_retrieve(cfg, "general", "format");
		if (!val)
			val = "wav";	
		ast_copy_string(vmfmts, val, sizeof(vmfmts));

		skipms = 3000;
		if ((val = ast_variable_retrieve(cfg, "general", "maxgreet"))) {
			if (sscanf(val, "%d", &x) == 1) {
				maxgreet = x;
			} else {
				ast_log(LOG_WARNING, "Invalid max message greeting length\n");
			}
		}

		if ((val = ast_variable_retrieve(cfg, "general", "skipms"))) {
			if (sscanf(val, "%d", &x) == 1) {
				skipms = x;
			} else {
				ast_log(LOG_WARNING, "Invalid skipms value\n");
			}
		}

		maxlogins = 3;
		if ((val = ast_variable_retrieve(cfg, "general", "maxlogins"))) {
			if (sscanf(val, "%d", &x) == 1) {
				maxlogins = x;
			} else {
				ast_log(LOG_WARNING, "Invalid max failed login attempts\n");
			}
		}

		/* Force new user to record name ? */
		if (!(val = ast_variable_retrieve(cfg, "general", "forcename"))) 
			val = "no";
		ast_set2_flag((&globalflags), ast_true(val), VM_FORCENAME);

		/* Force new user to record greetings ? */
		if (!(val = ast_variable_retrieve(cfg, "general", "forcegreetings"))) 
			val = "no";
		ast_set2_flag((&globalflags), ast_true(val), VM_FORCEGREET);

		if ((val = ast_variable_retrieve(cfg, "general", "cidinternalcontexts"))) {
			ast_debug(1, "VM_CID Internal context string: %s\n", val);
			stringp = ast_strdupa(val);
			for (x = 0; x < MAX_NUM_CID_CONTEXTS; x++) {
				if (!ast_strlen_zero(stringp)) {
					q = strsep(&stringp, ",");
					while ((*q == ' ')||(*q == '\t')) /* Eat white space between contexts */
						q++;
					ast_copy_string(cidinternalcontexts[x], q, sizeof(cidinternalcontexts[x]));
					ast_debug(1, "VM_CID Internal context %d: %s\n", x, cidinternalcontexts[x]);
				} else {
					cidinternalcontexts[x][0] = '\0';
				}
			}
		}
		if (!(val = ast_variable_retrieve(cfg, "general", "review"))) {
			ast_debug(1, "VM Review Option disabled globally\n");
			val = "no";
		}
		ast_set2_flag((&globalflags), ast_true(val), VM_REVIEW);	

		/* Temporary greeting reminder */
		if (!(val = ast_variable_retrieve(cfg, "general", "tempgreetwarn"))) {
			ast_debug(1, "VM Temporary Greeting Reminder Option disabled globally\n");
			val = "no";
		} else {
			ast_debug(1, "VM Temporary Greeting Reminder Option enabled globally\n");
		}
		ast_set2_flag((&globalflags), ast_true(val), VM_TEMPGREETWARN);

		if (!(val = ast_variable_retrieve(cfg, "general", "operator"))) {
			ast_debug(1, "VM Operator break disabled globally\n");
			val = "no";
		}
		ast_set2_flag((&globalflags), ast_true(val), VM_OPERATOR);	

		if (!(val = ast_variable_retrieve(cfg, "general", "saycid"))) {
			ast_debug(1, "VM CID Info before msg disabled globally\n");
			val = "no";
		} 
		ast_set2_flag((&globalflags), ast_true(val), VM_SAYCID);	

		if (!(val = ast_variable_retrieve(cfg, "general", "sendvoicemail"))) {
			ast_debug(1, "Send Voicemail msg disabled globally\n");
			val = "no";
		}
		ast_set2_flag((&globalflags), ast_true(val), VM_SVMAIL);
	
		if (!(val = ast_variable_retrieve(cfg, "general", "envelope"))) {
			ast_debug(1, "ENVELOPE before msg enabled globally\n");
			val = "yes";
		}
		ast_set2_flag((&globalflags), ast_true(val), VM_ENVELOPE);	

		if (!(val = ast_variable_retrieve(cfg, "general", "moveheard"))) {
			ast_debug(1, "Move Heard enabled globally\n");
			val = "yes";
		}
		ast_set2_flag((&globalflags), ast_true(val), VM_MOVEHEARD);	

		if (!(val = ast_variable_retrieve(cfg, "general", "sayduration"))) {
			ast_debug(1, "Duration info before msg enabled globally\n");
			val = "yes";
		}
		ast_set2_flag((&globalflags), ast_true(val), VM_SAYDURATION);	

		saydurationminfo = 2;
		if ((val = ast_variable_retrieve(cfg, "general", "saydurationm"))) {
			if (sscanf(val, "%d", &x) == 1) {
				saydurationminfo = x;
			} else {
				ast_log(LOG_WARNING, "Invalid min duration for say duration\n");
			}
		}

		if (!(val = ast_variable_retrieve(cfg, "general", "nextaftercmd"))) {
			ast_debug(1, "We are not going to skip to the next msg after save/delete\n");
			val = "no";
		}
		ast_set2_flag((&globalflags), ast_true(val), VM_SKIPAFTERCMD);

		if ((val = ast_variable_retrieve(cfg, "general", "dialout"))) {
			ast_copy_string(dialcontext, val, sizeof(dialcontext));
			ast_debug(1, "found dialout context: %s\n", dialcontext);
		} else {
			dialcontext[0] = '\0';	
		}
		
		if ((val = ast_variable_retrieve(cfg, "general", "callback"))) {
			ast_copy_string(callcontext, val, sizeof(callcontext));
			ast_debug(1, "found callback context: %s\n", callcontext);
		} else {
			callcontext[0] = '\0';
		}

		if ((val = ast_variable_retrieve(cfg, "general", "exitcontext"))) {
			ast_copy_string(exitcontext, val, sizeof(exitcontext));
			ast_debug(1, "found operator context: %s\n", exitcontext);
		} else {
			exitcontext[0] = '\0';
		}
		
		/* load password sounds configuration */
		if ((val = ast_variable_retrieve(cfg, "general", "vm-password")))
			ast_copy_string(vm_password, val, sizeof(vm_password));
		if ((val = ast_variable_retrieve(cfg, "general", "vm-newpassword")))
			ast_copy_string(vm_newpassword, val, sizeof(vm_newpassword));
		if ((val = ast_variable_retrieve(cfg, "general", "vm-passchanged")))
			ast_copy_string(vm_passchanged, val, sizeof(vm_passchanged));
		if ((val = ast_variable_retrieve(cfg, "general", "vm-reenterpassword")))
			ast_copy_string(vm_reenterpassword, val, sizeof(vm_reenterpassword));
		if ((val = ast_variable_retrieve(cfg, "general", "vm-mismatch")))
			ast_copy_string(vm_mismatch, val, sizeof(vm_mismatch));
		/* load configurable audio prompts */
		if ((val = ast_variable_retrieve(cfg, "general", "listen-control-forward-key")) && is_valid_dtmf(val))
			ast_copy_string(listen_control_forward_key, val, sizeof(listen_control_forward_key));
		if ((val = ast_variable_retrieve(cfg, "general", "listen-control-reverse-key")) && is_valid_dtmf(val))
			ast_copy_string(listen_control_reverse_key, val, sizeof(listen_control_reverse_key));
		if ((val = ast_variable_retrieve(cfg, "general", "listen-control-pause-key")) && is_valid_dtmf(val))
			ast_copy_string(listen_control_pause_key, val, sizeof(listen_control_pause_key));
		if ((val = ast_variable_retrieve(cfg, "general", "listen-control-restart-key")) && is_valid_dtmf(val))
			ast_copy_string(listen_control_restart_key, val, sizeof(listen_control_restart_key));
		if ((val = ast_variable_retrieve(cfg, "general", "listen-control-stop-key")) && is_valid_dtmf(val))
			ast_copy_string(listen_control_stop_key, val, sizeof(listen_control_stop_key));

		if (!(val = ast_variable_retrieve(cfg, "general", "usedirectory"))) 
			val = "no";
		ast_set2_flag((&globalflags), ast_true(val), VM_DIRECFORWARD);	

		poll_freq = DEFAULT_POLL_FREQ;
		if ((val = ast_variable_retrieve(cfg, "general", "pollfreq"))) {
			if (sscanf(val, "%u", &poll_freq) != 1) {
				poll_freq = DEFAULT_POLL_FREQ;
				ast_log(LOG_ERROR, "'%s' is not a valid value for the pollfreq option!\n", val);
			}
		}

		poll_mailboxes = 0;
		if ((val = ast_variable_retrieve(cfg, "general", "pollmailboxes")))
			poll_mailboxes = ast_true(val);

		if (ucfg) {	
			for (cat = ast_category_browse(ucfg, NULL); cat; cat = ast_category_browse(ucfg, cat)) {
				if (!ast_true(ast_config_option(ucfg, cat, "hasvoicemail")))
					continue;
				if ((cur = find_or_create(userscontext, cat))) {
					populate_defaults(cur);
					apply_options_full(cur, ast_variable_browse(ucfg, cat));
					ast_copy_string(cur->context, userscontext, sizeof(cur->context));
				}
			}
			ast_config_destroy(ucfg);
		}
		cat = ast_category_browse(cfg, NULL);
		while (cat) {
			if (strcasecmp(cat, "general")) {
				var = ast_variable_browse(cfg, cat);
				if (strcasecmp(cat, "zonemessages")) {
					/* Process mailboxes in this context */
					while (var) {
						append_mailbox(cat, var->name, var->value);
						var = var->next;
					}
				} else {
					/* Timezones in this context */
					while (var) {
						struct vm_zone *z;
						if ((z = ast_malloc(sizeof(*z)))) {
							char *msg_format, *timezone;
							msg_format = ast_strdupa(var->value);
							timezone = strsep(&msg_format, "|");
							if (msg_format) {
								ast_copy_string(z->name, var->name, sizeof(z->name));
								ast_copy_string(z->timezone, timezone, sizeof(z->timezone));
								ast_copy_string(z->msg_format, msg_format, sizeof(z->msg_format));
								AST_LIST_LOCK(&zones);
								AST_LIST_INSERT_HEAD(&zones, z, list);
								AST_LIST_UNLOCK(&zones);
							} else {
								ast_log(LOG_WARNING, "Invalid timezone definition at line %d\n", var->lineno);
								ast_free(z);
							}
						} else {
							AST_LIST_UNLOCK(&users);
							ast_config_destroy(cfg);
							return -1;
						}
						var = var->next;
					}
				}
			}
			cat = ast_category_browse(cfg, cat);
		}
		memset(fromstring, 0, sizeof(fromstring));
		memset(pagerfromstring, 0, sizeof(pagerfromstring));
		strcpy(charset, "ISO-8859-1");
		if (emailbody) {
			ast_free(emailbody);
			emailbody = NULL;
		}
		if (emailsubject) {
			ast_free(emailsubject);
			emailsubject = NULL;
		}
		if (pagerbody) {
			ast_free(pagerbody);
			pagerbody = NULL;
		}
		if (pagersubject) {
			ast_free(pagersubject);
			pagersubject = NULL;
		}
		if ((val = ast_variable_retrieve(cfg, "general", "pbxskip")))
			ast_set2_flag((&globalflags), ast_true(val), VM_PBXSKIP);
		if ((val = ast_variable_retrieve(cfg, "general", "fromstring")))
			ast_copy_string(fromstring, val, sizeof(fromstring));
		if ((val = ast_variable_retrieve(cfg, "general", "pagerfromstring")))
			ast_copy_string(pagerfromstring, val, sizeof(pagerfromstring));
		if ((val = ast_variable_retrieve(cfg, "general", "charset")))
			ast_copy_string(charset, val, sizeof(charset));
		if ((val = ast_variable_retrieve(cfg, "general", "adsifdn"))) {
			sscanf(val, "%2x%2x%2x%2x", &tmpadsi[0], &tmpadsi[1], &tmpadsi[2], &tmpadsi[3]);
			for (x = 0; x < 4; x++) {
				memcpy(&adsifdn[x], &tmpadsi[x], 1);
			}
		}
		if ((val = ast_variable_retrieve(cfg, "general", "adsisec"))) {
			sscanf(val, "%2x%2x%2x%2x", &tmpadsi[0], &tmpadsi[1], &tmpadsi[2], &tmpadsi[3]);
			for (x = 0; x < 4; x++) {
				memcpy(&adsisec[x], &tmpadsi[x], 1);
			}
		}
		if ((val = ast_variable_retrieve(cfg, "general", "adsiver"))) {
			if (atoi(val)) {
				adsiver = atoi(val);
			}
		}
		if ((val = ast_variable_retrieve(cfg, "general", "emailsubject")))
			emailsubject = ast_strdup(val);
		if ((val = ast_variable_retrieve(cfg, "general", "emailbody"))) {
			char *tmpread, *tmpwrite;
			emailbody = ast_strdup(val);

			/* substitute strings \t and \n into the appropriate characters */
			tmpread = tmpwrite = emailbody;
			while ((tmpwrite = strchr(tmpread, '\\'))) {
				switch (tmpwrite[1]) {
				case 'r':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\r';
					break;
				case 'n':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\n';
					break;
				case 't':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\t';
					break;
				default:
					ast_log(LOG_NOTICE, "Substitution routine does not support this character: %c\n", tmpwrite[1]);
				}
				tmpread = tmpwrite + 1;
			}
		}
		if ((val = ast_variable_retrieve(cfg, "general", "pagersubject")))
			pagersubject = ast_strdup(val);
		if ((val = ast_variable_retrieve(cfg, "general", "pagerbody"))) {
			char *tmpread, *tmpwrite;
			pagerbody = ast_strdup(val);

			/* substitute strings \t and \n into the appropriate characters */
			tmpread = tmpwrite = pagerbody;
			while ((tmpwrite = strchr(tmpread, '\\'))) {
				switch (tmpwrite[1]) {
				case 'r':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\r';
					break;
				case 'n':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\n';
					break;
				case 't':
					memmove(tmpwrite + 1, tmpwrite + 2, strlen(tmpwrite + 2) + 1);
					*tmpwrite = '\t';
					break;
				default:
					ast_log(LOG_NOTICE, "Substitution routine does not support this character: %c\n", tmpwrite[1]);
				}
				tmpread = tmpwrite + 1;
			}
		}
		AST_LIST_UNLOCK(&users);
		ast_config_destroy(cfg);

		if (poll_mailboxes && poll_thread == AST_PTHREADT_NULL)
			start_poll_thread();
		if (!poll_mailboxes && poll_thread != AST_PTHREADT_NULL)
			stop_poll_thread();;

		return 0;
	} else {
		AST_LIST_UNLOCK(&users);
		ast_log(LOG_WARNING, "Failed to load configuration file.\n");
		if (ucfg)
			ast_config_destroy(ucfg);
		return 0;
	}
}

static int reload(void)
{
	return load_config(1);
}

static int unload_module(void)
{
	int res;

	res = ast_unregister_application(app);
	res |= ast_unregister_application(app2);
	res |= ast_unregister_application(app3);
	res |= ast_unregister_application(app4);
	res |= ast_custom_function_unregister(&mailbox_exists_acf);
	res |= ast_manager_unregister("VoicemailUsersList");
	ast_cli_unregister_multiple(cli_voicemail, sizeof(cli_voicemail) / sizeof(struct ast_cli_entry));
	ast_uninstall_vm_functions();

	if (poll_thread != AST_PTHREADT_NULL)
		stop_poll_thread();


	free_vm_users();
	free_vm_zones();
	return res;
}

static int load_module(void)
{
	int res;
	my_umask = umask(0);
	umask(my_umask);

	/* compute the location of the voicemail spool directory */
	snprintf(VM_SPOOL_DIR, sizeof(VM_SPOOL_DIR), "%s/voicemail/", ast_config_AST_SPOOL_DIR);

	if ((res = load_config(0)))
		return res;

	res = ast_register_application(app, vm_exec, synopsis_vm, descrip_vm);
	res |= ast_register_application(app2, vm_execmain, synopsis_vmain, descrip_vmain);
	res |= ast_register_application(app3, vm_box_exists, synopsis_vm_box_exists, descrip_vm_box_exists);
	res |= ast_register_application(app4, vmauthenticate, synopsis_vmauthenticate, descrip_vmauthenticate);
	res |= ast_custom_function_register(&mailbox_exists_acf);
	res |= ast_manager_register("VoicemailUsersList", EVENT_FLAG_CALL | EVENT_FLAG_REPORTING, manager_list_voicemail_users, "List All Voicemail User Information");
	if (res)
		return res;

	ast_cli_register_multiple(cli_voicemail, sizeof(cli_voicemail) / sizeof(struct ast_cli_entry));

	ast_install_vm_functions(has_voicemail, inboxcount, messagecount);

	return res;
}

static int dialout(struct ast_channel *chan, struct ast_vm_user *vmu, char *num, char *outgoing_context) 
{
	int cmd = 0;
	char destination[80] = "";
	int retries = 0;

	if (!num) {
		ast_verb(3, "Destination number will be entered manually\n");
		while (retries < 3 && cmd != 't') {
			destination[1] = '\0';
			destination[0] = cmd = ast_play_and_wait(chan, "vm-enter-num-to-call");
			if (!cmd)
				destination[0] = cmd = ast_play_and_wait(chan, "vm-then-pound");
			if (!cmd)
				destination[0] = cmd = ast_play_and_wait(chan, "vm-star-cancel");
			if (!cmd) {
				cmd = ast_waitfordigit(chan, 6000);
				if (cmd)
					destination[0] = cmd;
			}
			if (!cmd) {
				retries++;
			} else {

				if (cmd < 0)
					return 0;
				if (cmd == '*') {
					ast_verb(3, "User hit '*' to cancel outgoing call\n");
					return 0;
				}
				if ((cmd = ast_readstring(chan, destination + strlen(destination), sizeof(destination)-1, 6000, 10000, "#")) < 0) 
					retries++;
				else
					cmd = 't';
			}
		}
		if (retries >= 3) {
			return 0;
		}
		
	} else {
		ast_verb(3, "Destination number is CID number '%s'\n", num);
		ast_copy_string(destination, num, sizeof(destination));
	}

	if (!ast_strlen_zero(destination)) {
		if (destination[strlen(destination) -1 ] == '*')
			return 0; 
		ast_verb(3, "Placing outgoing call to extension '%s' in context '%s' from context '%s'\n", destination, outgoing_context, chan->context);
		ast_copy_string(chan->exten, destination, sizeof(chan->exten));
		ast_copy_string(chan->context, outgoing_context, sizeof(chan->context));
		chan->priority = 0;
		return 9;
	}
	return 0;
}

static int advanced_options(struct ast_channel *chan, struct ast_vm_user *vmu, struct vm_state *vms, int msg, int option, signed char record_gain)
{
	int res = 0;
	char filename[PATH_MAX];
	struct ast_config *msg_cfg = NULL;
	const char *origtime, *context;
	char *name, *num;
	int retries = 0;
	char *cid;
	struct ast_flags config_flags = { CONFIG_FLAG_NOCACHE, };

	vms->starting = 0; 

	make_file(vms->fn, sizeof(vms->fn), vms->curdir, msg);

	/* Retrieve info from VM attribute file */

	make_file(vms->fn2, sizeof(vms->fn2), vms->curdir, vms->curmsg);
	snprintf(filename, sizeof(filename), "%s.txt", vms->fn2);
	RETRIEVE(vms->curdir, vms->curmsg, vmu->mailbox, vmu->context);
	msg_cfg = ast_config_load(filename, config_flags);
	DISPOSE(vms->curdir, vms->curmsg);
	if (!msg_cfg) {
		ast_log(LOG_WARNING, "No message attribute file?!! (%s)\n", filename);
		return 0;
	}

	if (!(origtime = ast_variable_retrieve(msg_cfg, "message", "origtime"))) {
		ast_config_destroy(msg_cfg);
		return 0;
	}

	cid = ast_strdupa(ast_variable_retrieve(msg_cfg, "message", "callerid"));

	context = ast_variable_retrieve(msg_cfg, "message", "context");
	if (!strncasecmp("macro", context, 5)) /* Macro names in contexts are useless for our needs */
		context = ast_variable_retrieve(msg_cfg, "message", "macrocontext");
	switch (option) {
	case 3:
		if (!res)
			res = play_message_datetime(chan, vmu, origtime, filename);
		if (!res)
			res = play_message_callerid(chan, vms, cid, context, 0);

		res = 't';
		break;

	case 2:	/* Call back */

		if (ast_strlen_zero(cid))
			break;

		ast_callerid_parse(cid, &name, &num);
		while ((res > -1) && (res != 't')) {
			switch (res) {
			case '1':
				if (num) {
					/* Dial the CID number */
					res = dialout(chan, vmu, num, vmu->callback);
					if (res) {
						ast_config_destroy(msg_cfg);
						return 9;
					}
				} else {
					res = '2';
				}
				break;

			case '2':
				/* Want to enter a different number, can only do this if there's a dialout context for this user */
				if (!ast_strlen_zero(vmu->dialout)) {
					res = dialout(chan, vmu, NULL, vmu->dialout);
					if (res) {
						ast_config_destroy(msg_cfg);
						return 9;
					}
				} else {
					ast_verb(3, "Caller can not specify callback number - no dialout context available\n");
					res = ast_play_and_wait(chan, "vm-sorry");
				}
				ast_config_destroy(msg_cfg);
				return res;
			case '*':
				res = 't';
				break;
			case '3':
			case '4':
			case '5':
			case '6':
			case '7':
			case '8':
			case '9':
			case '0':

				res = ast_play_and_wait(chan, "vm-sorry");
				retries++;
				break;
			default:
				if (num) {
					ast_verb(3, "Confirm CID number '%s' is number to use for callback\n", num);
					res = ast_play_and_wait(chan, "vm-num-i-have");
					if (!res)
						res = play_message_callerid(chan, vms, num, vmu->context, 1);
					if (!res)
						res = ast_play_and_wait(chan, "vm-tocallnum");
					/* Only prompt for a caller-specified number if there is a dialout context specified */
					if (!ast_strlen_zero(vmu->dialout)) {
						if (!res)
							res = ast_play_and_wait(chan, "vm-calldiffnum");
					}
				} else {
					res = ast_play_and_wait(chan, "vm-nonumber");
					if (!ast_strlen_zero(vmu->dialout)) {
						if (!res)
							res = ast_play_and_wait(chan, "vm-toenternumber");
					}
				}
				if (!res)
					res = ast_play_and_wait(chan, "vm-star-cancel");
				if (!res)
					res = ast_waitfordigit(chan, 6000);
				if (!res) {
					retries++;
					if (retries > 3)
						res = 't';
				}
				break; 
				
			}
			if (res == 't')
				res = 0;
			else if (res == '*')
				res = -1;
		}
		break;
		
	case 1:	/* Reply */
		/* Send reply directly to sender */
		if (ast_strlen_zero(cid))
			break;

		ast_callerid_parse(cid, &name, &num);
		if (!num) {
			ast_verb(3, "No CID number available, no reply sent\n");
			if (!res)
				res = ast_play_and_wait(chan, "vm-nonumber");
			ast_config_destroy(msg_cfg);
			return res;
		} else {
			struct ast_vm_user vmu2;
			if (find_user(&vmu2, vmu->context, num)) {
				struct leave_vm_options leave_options;
				char mailbox[AST_MAX_EXTENSION * 2 + 2];
				snprintf(mailbox, sizeof(mailbox), "%s@%s", num, vmu->context);

				ast_verb(3, "Leaving voicemail for '%s' in context '%s'\n", num, vmu->context);
				
				memset(&leave_options, 0, sizeof(leave_options));
				leave_options.record_gain = record_gain;
				res = leave_voicemail(chan, mailbox, &leave_options);
				if (!res)
					res = 't';
				ast_config_destroy(msg_cfg);
				return res;
			} else {
				/* Sender has no mailbox, can't reply */
				ast_verb(3, "No mailbox number '%s' in context '%s', no reply sent\n", num, vmu->context);
				ast_play_and_wait(chan, "vm-nobox");
				res = 't';
				ast_config_destroy(msg_cfg);
				return res;
			}
		} 
		res = 0;

		break;
	}

#ifndef IMAP_STORAGE
	ast_config_destroy(msg_cfg);

	if (!res) {
		make_file(vms->fn, sizeof(vms->fn), vms->curdir, msg);
		vms->heard[msg] = 1;
		res = wait_file(chan, vms, vms->fn);
	}
#endif
	return res;
}

static int play_record_review(struct ast_channel *chan, char *playfile, char *recordfile, int maxtime, char *fmt,
			int outsidecaller, struct ast_vm_user *vmu, int *duration, const char *unlockdir,
			signed char record_gain, struct vm_state *vms)
{
	/* Record message & let caller review or re-record it, or set options if applicable */
	int res = 0;
	int cmd = 0;
	int max_attempts = 3;
	int attempts = 0;
	int recorded = 0;
	int message_exists = 0;
	signed char zero_gain = 0;
	char tempfile[PATH_MAX];
	char *acceptdtmf = "#";
	char *canceldtmf = "";

	/* Note that urgent and private are for flagging messages as such in the future */

	/* barf if no pointer passed to store duration in */
	if (duration == NULL) {
		ast_log(LOG_WARNING, "Error play_record_review called without duration pointer\n");
		return -1;
	}

	if (!outsidecaller)
		snprintf(tempfile, sizeof(tempfile), "%s.tmp", recordfile);
	else
		ast_copy_string(tempfile, recordfile, sizeof(tempfile));

	cmd = '3';  /* Want to start by recording */

	while ((cmd >= 0) && (cmd != 't')) {
		switch (cmd) {
		case '1':
			if (!message_exists) {
				/* In this case, 1 is to record a message */
				cmd = '3';
				break;
			} else {
				/* Otherwise 1 is to save the existing message */
				ast_verb(3, "Saving message as is\n");
				if (!outsidecaller) 
					ast_filerename(tempfile, recordfile, NULL);
				ast_stream_and_wait(chan, "vm-msgsaved", "");
				if (!outsidecaller) {
					STORE(recordfile, vmu->mailbox, vmu->context, -1, chan, vmu, fmt, *duration, vms);
					DISPOSE(recordfile, -1);
				}
				cmd = 't';
				return res;
			}
		case '2':
			/* Review */
			ast_verb(3, "Reviewing the message\n");
			cmd = ast_stream_and_wait(chan, tempfile, AST_DIGIT_ANY);
			break;
		case '3':
			message_exists = 0;
			/* Record */
			if (recorded == 1) 
				ast_verb(3, "Re-recording the message\n");
			else	
				ast_verb(3, "Recording the message\n");
			
			if (recorded && outsidecaller) {
				cmd = ast_play_and_wait(chan, INTRO);
				cmd = ast_play_and_wait(chan, "beep");
			}
			recorded = 1;
			/* After an attempt has been made to record message, we have to take care of INTRO and beep for incoming messages, but not for greetings */
			if (record_gain)
				ast_channel_setoption(chan, AST_OPTION_RXGAIN, &record_gain, sizeof(record_gain), 0);
			if (ast_test_flag(vmu, VM_OPERATOR))
				canceldtmf = "0";
			cmd = ast_play_and_record_full(chan, playfile, tempfile, maxtime, fmt, duration, silencethreshold, maxsilence, unlockdir, acceptdtmf, canceldtmf);
			if (record_gain)
				ast_channel_setoption(chan, AST_OPTION_RXGAIN, &zero_gain, sizeof(zero_gain), 0);
			if (cmd == -1) {
				/* User has hung up, no options to give */
				if (!outsidecaller) {
					/* user was recording a greeting and they hung up, so let's delete the recording. */
					ast_filedelete(tempfile, NULL);
				}		
				return cmd;
			}
			if (cmd == '0') {
				break;
			} else if (cmd == '*') {
				break;
#if 0
			} else if (vmu->review && (*duration < 5)) {
				/* Message is too short */
				ast_verb(3, "Message too short\n");
				cmd = ast_play_and_wait(chan, "vm-tooshort");
				cmd = ast_filedelete(tempfile, NULL);
				break;
			} else if (vmu->review && (cmd == 2 && *duration < (maxsilence + 3))) {
				/* Message is all silence */
				ast_verb(3, "Nothing recorded\n");
				cmd = ast_filedelete(tempfile, NULL);
				cmd = ast_play_and_wait(chan, "vm-nothingrecorded");
				if (!cmd)
					cmd = ast_play_and_wait(chan, "vm-speakup");
				break;
#endif
			} else {
				/* If all is well, a message exists */
				message_exists = 1;
				cmd = 0;
			}
			break;
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '*':
		case '#':
			cmd = ast_play_and_wait(chan, "vm-sorry");
			break;
#if 0 
/*  XXX Commented out for the moment because of the dangers of deleting
    a message while recording (can put the message numbers out of sync) */
		case '*':
			/* Cancel recording, delete message, offer to take another message*/
			cmd = ast_play_and_wait(chan, "vm-deleted");
			cmd = ast_filedelete(tempfile, NULL);
			if (outsidecaller) {
				res = vm_exec(chan, NULL);
				return res;
			}
			else
				return 1;
#endif
		case '0':
			if (!ast_test_flag(vmu, VM_OPERATOR)) {
				cmd = ast_play_and_wait(chan, "vm-sorry");
				break;
			}
			if (message_exists || recorded) {
				cmd = ast_play_and_wait(chan, "vm-saveoper");
				if (!cmd)
					cmd = ast_waitfordigit(chan, 3000);
				if (cmd == '1') {
					ast_play_and_wait(chan, "vm-msgsaved");
					cmd = '0';
				} else {
					ast_play_and_wait(chan, "vm-deleted");
					DELETE(recordfile, -1, recordfile, vmu);
					cmd = '0';
				}
			}
			return cmd;
		default:
			/* If the caller is an ouside caller, and the review option is enabled,
			   allow them to review the message, but let the owner of the box review
			   their OGM's */
			if (outsidecaller && !ast_test_flag(vmu, VM_REVIEW))
				return cmd;
			if (message_exists) {
				cmd = ast_play_and_wait(chan, "vm-review");
			} else {
				cmd = ast_play_and_wait(chan, "vm-torerecord");
				if (!cmd)
					cmd = ast_waitfordigit(chan, 600);
			}
			
			if (!cmd && outsidecaller && ast_test_flag(vmu, VM_OPERATOR)) {
				cmd = ast_play_and_wait(chan, "vm-reachoper");
				if (!cmd)
					cmd = ast_waitfordigit(chan, 600);
			}
#if 0
			if (!cmd)
				cmd = ast_play_and_wait(chan, "vm-tocancelmsg");
#endif
			if (!cmd)
				cmd = ast_waitfordigit(chan, 6000);
			if (!cmd) {
				attempts++;
			}
			if (attempts > max_attempts) {
				cmd = 't';
			}
		}
	}
	if (outsidecaller)
		ast_play_and_wait(chan, "vm-goodbye");
	if (cmd == 't')
		cmd = 0;
	return cmd;
}

/* This is a workaround so that menuselect displays a proper description
 * AST_MODULE_INFO(, , "Comedian Mail (Voicemail System)"
 */

AST_MODULE_INFO(ASTERISK_GPL_KEY, AST_MODFLAG_DEFAULT, tdesc,
		.load = load_module,
		.unload = unload_module,
		.reload = reload,
		);
