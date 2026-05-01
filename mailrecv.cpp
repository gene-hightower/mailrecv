// vim: autoindent tabstop=8 shiftwidth=4 expandtab softtabstop=4

//
// mailrecv.cpp -- xinetd tool to act as a simple SMTP server
//
//     Internet facing mail server that directs letters to valid
//     local recipients either directly to a file or a command pipe
//     based on RCPT TO: address.
//
// Copyright 2018 Greg Ercolano
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public Licensse as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
//
// 80 //////////////////////////////////////////////////////////////////////////

#include <stdio.h>
#include <string.h>     // strchr()
#include <errno.h>      // errno
#include <stdlib.h>     // exit()
#include <unistd.h>     // sleep()
#include <stdarg.h>     // vargs
#include <syslog.h>     // syslog()
#include <pcre.h>       // perl regex API (see 'man pcreapi(3)')
#include <sys/socket.h> // getpeername()
#include <netdb.h>      // gethostbyaddr(), NI_MAXHOST..
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/file.h>   // flock()
#include <pthread.h>    // pthread_create() for execution timer
#include <string>
#include <vector>
#include <sstream>

using namespace std;

// SMTP command bit flags
//     One per command received, used mainly for logging
//
enum {
    SMTP_CMD_QUIT = 0x0001, SMTP_CMD_HELO = 0x0002, SMTP_CMD_MAIL = 0x0004, SMTP_CMD_RCPT = 0x0008,
    SMTP_CMD_DATA = 0x0010, SMTP_CMD_VRFY = 0x0020, SMTP_CMD_RSET = 0x0040, SMTP_CMD_NOOP = 0x0080,
    SMTP_CMD_HELP = 0x0100, SMTP_CMD_EXPN = 0x0200, SMTP_CMD_SEND = 0x0400, SMTP_CMD_SOML = 0x0800,
    SMTP_CMD_SAML = 0x1000, SMTP_CMD_TURN = 0x2000, SMTP_CMD_EHLO = 0x4000
};

#define LINE_LEN        4096
#define CRLF            "\r\n"            // RFC 821 (GLOSSARY) / RFC 822 (APPENDIX D)
#define CONFIG_FILE     "/etc/mailrecv.conf"
#define PROGNAME        "MAILRECV"

// Check for log flags
#define ISLOG(s) if (G_debugflags[0] && (G_debugflags[0]=='a'||strpbrk(G_debugflags,s)))

///// GLOBALS /////
const char *G_debugflags = "";            // debug logging flags (see mailrecv.conf for description)
char        G_remotehost[256];            // Remote's hostname
char        G_remoteip[NI_MAXHOST];       // Remote's IP address
char       *G_logfilename = NULL;         // log filename if configured (if NULL, uses syslog)
FILE       *G_logfp = NULL;               // log file pointer (remains open for duration of process)

// LOCK THE LOG FILE FOR WRITING/ROTATING
//    Lock the log for pthread safety.
//    Returns -1 on error, error msg sent to stderr
//
int LogLock() {
    if ( !G_logfp || G_logfp == stderr ) return(0); // reasons /not/ to lock
    if ( flock(fileno(G_logfp), LOCK_EX) < 0 ) {
        fprintf(stderr, "%s: LogLock(): flock(LOCK_EX): %s", PROGNAME, strerror(errno));
	return(-1);
    }
    return(0);
}

// UNLOCK THE LOG FILE
//    Returns -1 on error, error msg sent to stderr
//
int LogUnlock() {
    if ( !G_logfp || G_logfp == stderr ) return(0); // reasons /not/ to lock
    if ( flock(fileno(G_logfp), LOCK_UN) < 0 ) {
        fprintf(stderr, "%s: LogUnlock(): flock(LOCK_UN): %s", PROGNAME, strerror(errno));
	return(-1);
    }
    return(0);
}

// Log prefix: each line in log prefixed by this string (date/time/etc)
void GetLogPrefix(string &return_msg) {
    // Get current time/date as a string
    time_t    secs;		// Current UNIX time
    struct tm *date;		// Current date/time
    char      datestr[1024];	// Date/time string
    time(&secs);
    date = localtime(&secs);
    strftime(datestr, sizeof(datestr), "%c", date);

    ostringstream os;
    // Log date/time/pid
    os << datestr << " " << PROGNAME << "_V" << VERSION << "[" << getpid() << "]: ";
    // Fail2ban filtering: show remote ip after pid in every line of log output
    ISLOG("F") os << "[" << G_remoteip << "] ";
    // Return result
    return_msg = os.str();
}

// Log a message...
//     Note: msg can contain '%m', which is replaced with strerror(errno)
//     You MUST include a trailing \n in msg for consistent logging.
//
void Log(const char *msg, ...) {
    va_list ap;
    va_start(ap, msg);

    if ( !G_logfilename ) {         // No logfile specified?
        vsyslog(LOG_ERR, msg, ap);  // Use syslog..
        va_end(ap);
        return;
    }

    LogLock();
/*LCK*/ // Logfile specified? Append caller's msg to that file
/*LCK*/ if ( G_logfp == NULL ) {     // Log not open yet? Open it first
/*LCK*/     int prev_errno = errno;  // save previous errno first (in case caller using %m)
/*LCK*/     if ( (G_logfp = fopen(G_logfilename, "a")) == NULL ) {  // open logfile
/*LCK*/         syslog(LOG_ERR, "%s: %m", G_logfilename);           // Error? use syslog to log file error, and..
/*LCK*/         errno = prev_errno;                                 // restore errno for caller (in case %m used), and..
/*LCK*/         vsyslog(LOG_ERR, msg, ap);                          // log caller's error last
/*LCK*/         va_end(ap);
/*LCK*/         LogUnlock();
/*LCK*/         return;
/*LCK*/     }
/*LCK*/     errno = prev_errno;
/*LCK*/ }
/*LCK*/
/*LCK*/ // Log prefix
/*LCK*/ {
/*LCK*/     string logprefix;
/*LCK*/     GetLogPrefix(logprefix);
/*LCK*/     fprintf(G_logfp, "%s", logprefix.c_str());
/*LCK*/ }
/*LCK*/ vfprintf(G_logfp, msg, ap); // append caller's error to logfile
/*LCK*/ fflush(G_logfp);            // flush after each line
    LogUnlock();
    va_end(ap);
}

// Return ASCII only version of string 's', with binary encoded as hex <0x##>
//     NOTE: in the following, "ASCII" is defined as per RFC 822 4.1.2.
//
char *AsciiHexEncode(const char *s, int allow_crlf=0) {
    // First pass: determine how large output string needs to be
    int outlen = 0;
    const char *ss = s;
    while ( *ss ) {
        if ( *ss >= 0x20 && *ss <= 0x7e )   // Printable ASCII? (RFC 822 4.1.2)
            { ++outlen; }                   // OK
        else if ( allow_crlf && (*ss == '\r' || *ss == '\n') ) // CRLF allowed?
            { ++outlen; }                   // OK
        else
            { outlen += 6; }                // 6 chars for every one binary char
        ++ss;
    }
    ++outlen; // leave room for terminating NULL
    char *buf = (char*)malloc(outlen);
    char *out = buf;
    ss = s;
    while ( *ss ) {
        if ( *ss >= 0x20 && *ss <= 0x7e )   // Printable ASCII?
            { *out++ = *ss; }               // OK
        else if ( allow_crlf && (*ss == '\r' || *ss == '\n') ) // CRLF allowed?
            { *out++ = *ss; }               // OK
        else {
            sprintf(out, "<0x%02x>", (unsigned char)*ss);  // write hex code
            out += 6;                       // move past hex code
        }
        ++ss;
    }
    *out = 0;
    return buf;
}

// Check if string 's' contains any binary data, return 1 if so.
//     NOTE: Binary is defined as any character not allowed by RFC 822 4.1.2.
//
int BinaryCheck(const char *s, int allow_crlf=0) {
    while ( *s ) {
        if ( *s >= 0x20 && *s <= 0x7e )                 // Printable ASCII?
            { ++s; continue; }                          // ..OK
        if ( allow_crlf && (*s == '\r' || *s == '\n') ) // CRLF allowed?
            { ++s; continue; }                          // ..OK
        return 1;                                       // binary? return 1
    }
    return 0;                                           // no binary? return 0
}

// Handle sending a reply back to the server with added CRLF
void SMTP_Reply(const char *s) {
    printf("%s%s", s, CRLF);
    fflush(stdout);
    ISLOG("s") Log("DEBUG: SMTP reply: %s\n", s);
}

// Do a regular expression match test
//
//     regex[in] -- regular expression to match against string
//     match[in] -- string to be matched
//
// Returns:
//     1: string matched
//     0: string didn't match
//    -1: an error occurred (reason was printed to stderr)
//
int RegexMatch(const char*regex, const char *match) {
    const char *regex_errorstr;         // returned error if any
    int         regex_erroroff;         // offset in string where error occurred

    // Compile the regex..
    pcre *regex_compiled = pcre_compile(regex, 0, &regex_errorstr, &regex_erroroff, NULL);
    if ( regex_compiled == NULL ) {
        Log("ERROR: could not compile regex '%s': %s\n", regex, regex_errorstr);
        Log("                               %*s^\n",     regex_erroroff, ""); // point to the error
        Log("                               %*sError here\n", regex_erroroff, "");
        return -1;
    }

    // Optimize regex
    pcre_extra *regex_extra = pcre_study(regex_compiled, 0, &regex_errorstr);
    if ( regex_errorstr != NULL ) {
        pcre_free(regex_compiled);  // don't leak compiled regex
        Log("ERROR: Could not study regex '%s': %s\n", regex, regex_errorstr);
        return -1;
    }

    // Now see if we can match string
    int *substrvec = NULL; // pcre_exec()'s captured substrings (NULL=disinterest)
    int nsubstrvec = 0;    // number of elements in substrvec (0=disinterest)
    int soff = 0;          // starting offset (0=start of string)
    int opts = 0;          // pcre_exec()'s options (0=none)
    int ret = pcre_exec(regex_compiled, regex_extra, match, strlen(match), soff, opts, substrvec, nsubstrvec);

    // Free up compiled regex
    pcre_free(regex_compiled);
    pcre_free(regex_extra);

    // Check match results..
    if ( ret < 0 ) {
        switch (ret) {
            case PCRE_ERROR_NOMATCH:
                return 0;  // string didn't match
            default:
                Log("ERROR: bad regex '%s'\n", regex);
                return -1;
        }
    }
    return 1;   // string matched
}

// Append email to the specified file
int AppendMailToFile(const char *mail_from,         // SMTP 'mail from:'
                     const char *rcpt_to,           // SMTP 'rcpt to:'
                     const vector<string>& letter,  // email contents, including headers, blank line, body
                     const string& filename) {      // filename to append to
    bool locked = false;
    FILE *fp;
    // Open file for append
    ISLOG("f") Log("DEBUG: fopen(%s,'a')\n", filename.c_str());
    if ( (fp = fopen(filename.c_str(), "a")) == NULL) {
        Log("ERROR: can't append to %s: %m\n", filename.c_str());   // %m: see syslog(3)
        return -1;  // fail
    }
    // Lock file
    if ( flock(fileno(fp), LOCK_EX) == 0 ) {
        locked = true;
    } else {
        fprintf(stderr, "%s: AppendMailToFile(): flock(LOCK_EX): %s", PROGNAME, strerror(errno));
        locked = false; // continue anyway
    }

/*LCK*/    // Append letter
/*LCK*/    fprintf(fp, "From %s\n", mail_from);
/*LCK*/    for ( size_t t=0; t<letter.size(); t++ ) {
/*LCK*/        fprintf(fp, "%s\n", letter[t].c_str());
/*LCK*/    }
/*LCK*/    fprintf(fp, "\n");

    // Unlock (if locked)
    if ( locked && flock(fileno(fp), LOCK_UN) < 0 ) {
        fprintf(stderr, "%s: AppendMailToFile(): flock(LOCK_UN): %s", PROGNAME, strerror(errno));
    }
    // Close file
    int ret = fclose(fp);
    ISLOG("f") Log("DEBUG: fclose() returned %d\n", ret);

    return 1;       // success
}

// Pipe letter to specified shell command
int PipeMailToCommand(const char *mail_from,        // SMTP 'mail from:'
                      const char *rcpt_to,          // SMTP 'rcpt to:'
                      const vector<string>& letter, // email contents, including headers, blank line, body
                      const string& command) {      // unix shell command to write to
    ISLOG("f") Log("DEBUG: popen(%s,'w')..\n", command.c_str());
    FILE *fp;
    if ( (fp = popen(command.c_str(), "w")) == NULL) {
        Log("ERROR: can't popen(%s): %m\n", command.c_str());
        return -1;  // fail
    }
    fprintf(fp, "From %s\n", mail_from);            // XXX: might not be needed
    for ( size_t t=0; t<letter.size(); t++ ) {
        fprintf(fp, "%s\n", letter[t].c_str());
    }
    int ret = pclose(fp);
    ISLOG("f") Log("DEBUG: pclose() returned %d\n", ret);
    return 1;       // success
}

// Isolate the email address
//     "Foo Bar <foo@bar.com>" -> "foo@bar.com"
//     "<foo@bar.com>" -> "foo@bar.com"
//     "foo@bar.com" -> "foo@bar.com"
//
void IsolateAddress(char* s) {
    char *p = s;
    // Skip leading white
    while ( *p == ' ' || *p == '\t' ) p++;
    // Has angle brackets?
    //     Could be "Full Name <a@b>" or "<a@b>" or "<a@b" or "<<<a@b>"..
    //
    if ( strchr(p, '<') ) {              // any '<'s?
        p = strchr(p, '<');              // skip possible "Full Name"
        while ( *p ) {                   // parse up to closing '>'
            if ( *p == '<' ) { ++p; }    // skip /all/ '<'s
            else if ( *p == '>' ) break; // stop at first '>'
            else *s++ = *p++;
        }
        *s = 0;
        return;
    } else {
        // No leading angle bracket?
        //     Isolated address ("a@b") or malformed ("a@b>")
        //
        while ( *p ) {
            if ( *p == '>' ) break;     // "a@b>" -> "a@b"
            *s++ = *p++;
        }
        *s = 0;                         // eol
        return;
    }
}

// Class to manage a group of regex patterns
struct AllowGroup {
    string name;                // group name, e.g. "+aservers"
    vector<string> regexes;     // array of regex patterns to match (e.g. "mail[1234].server.com")
};

// mailrecv's configuration file class
//     TODO: This should be moved to a separate file.
//
class Configure {
    char loghex;                                    // 1=log binary chars in HEX, 0=no hex translation
    string domain;                                  // domain our server should know itself as (e.g. "example.com")
                                                    // and accept email messages for.
    string deadletter_file;                         // file to append messages to that have no 'deliver'

    // Limits..
    long limit_smtp_commands;          // limit on # smtp commands per session
    long limit_smtp_unknowncmd;        // limit on # unknown smtp commands per session
    long limit_smtp_failcmds;          // limit on # failed smtp commands
    long limit_connection_secs;        // limit connection time (in secs)
    long limit_smtp_data_size;         // limit on #bytes DATA command can receive
    long limit_smtp_rcpt_to;           // limit on # "RCPT TO:" commands we can receive
    int  limit_smtp_ascii;             // limit on smtp commands+args to ascii only content
    // Error strings for each limit..
    string limit_smtp_commands_emsg;   // limit on # smtp commands per session
    string limit_smtp_unknowncmd_emsg; // limit on # unknown smtp commands per session
    string limit_smtp_failcmds_emsg;   // limit on # failed smtp commands
    string limit_connection_secs_emsg; // limit connection time (in secs)
    string limit_smtp_data_size_emsg;  // limit on #bytes DATA command can receive
    string limit_smtp_rcpt_to_emsg;    // limit on # "RCPT TO:" commands we can receive
    string limit_smtp_ascii_emsg;      // limit on smtp commands+args to ascii only content

    vector<AllowGroup> allowgroups;                 // "allow groups"
    vector<string> deliver_rcpt_to_pipe_allowgroups;// hosts allowed to send to this address
    vector<string> deliver_rcpt_to_pipe_address;    // configured rcpt_to addresses to pipe to a shell command (TODO: Should be regex instead?)
    vector<string> deliver_rcpt_to_pipe_command;    // rcpt_to shell command to pipe matching mail to address

    vector<string> deliver_rcpt_to_file_allowgroups;// hosts allowed to send to this address
    vector<string> deliver_rcpt_to_file_address;    // rcpt_to file addresses we allow (TODO: Should be regex instead?)
    vector<string> deliver_rcpt_to_file_filename;   // rcpt_to file filename we append letters to

    //NO vector<string> errors_rcpt_to_allowgroups; // we don't need this; always OK to send remote an error ;)
    vector<string> errors_rcpt_to_regex;            // error address to match
    vector<string> errors_rcpt_to_message;          // error message to send remote on match

    vector<string> replace_rcpt_to_regex;           // rcpt_to regex to search for            (TODO: NOT YET IMPLEMENTED)
    vector<string> replace_rcpt_to_after;           // rcpt_to regex match replacement string (TODO: NOT YET IMPLEMENTED)

    vector<string> allow_remotehost_regex;          // allowed remotehost regex

public:
    Configure() {
        loghex     = 0;                             // loghex [default: off]
        domain     = "example.com";
        deadletter_file = "/dev/null";              // must be set to "something"
        limit_smtp_commands        = 25;
        limit_smtp_commands_emsg   = "500 Too many SMTP commands received in session.";
        limit_smtp_unknowncmd      = 4;
        limit_smtp_unknowncmd_emsg = "500 Too many bad commands.";
        limit_smtp_failcmds        = 4;
        limit_smtp_failcmds_emsg   = "500 Too many failed commands.";
        limit_connection_secs      = 600;
        limit_connection_secs_emsg = "500 Connection timeout.";
        limit_smtp_data_size       = 24000000;
        limit_smtp_data_size_emsg  = "552 Too much mail data.";
        limit_smtp_rcpt_to         = 5;
        limit_smtp_rcpt_to_emsg    = "452 Too many recipients.";    // RFC 2821 4.5.3.1
        limit_smtp_ascii           = OnOff("on");                   // ascii-only smtp cmd strings [default: on]
        limit_smtp_ascii_emsg      = "500 Binary data (non-ASCII) unsupported.";
    }

    // Accessors
    int LogHex() const { return loghex; }
    int LimitSmtpAscii() const { return limit_smtp_ascii; }
    const char *Domain() const { return domain.c_str(); }
    const char *DeadLetterFile() const { return deadletter_file.c_str(); }

    // See if string is "on" or "off" (or similar values)
    //    Returns -1 if unknown string
    //
    int OnOff(const char *s) {
        if ( strcmp(s, "yes" ) == 0 ) return 1;
        if ( strcmp(s, "on"  ) == 0 ) return 1;
        if ( strcmp(s, "1"   ) == 0 ) return 1;
        if ( strcmp(s, "no"  ) == 0 ) return 0;
        if ( strcmp(s, "off" ) == 0 ) return 0;
        if ( strcmp(s, "0"   ) == 0 ) return 0;
        return -1;
    }

    // Limit checks
    // Returns:
    //     0 -- if OK.
    //    -1 -- if limit reached, emsg has error to send remote.
    //
    int CheckLimit(long val, string limit_name, string& emsg) {
        if ( limit_name == "smtp_commands" ) {
            if ( val < limit_smtp_commands ) return 0;
            emsg = limit_smtp_commands_emsg;
            return -1; 
        } else if ( limit_name == "smtp_unknowncmd" ) {
            if ( val < limit_smtp_unknowncmd ) return 0;
            emsg = limit_smtp_unknowncmd_emsg;
            return -1; 
        } else if ( limit_name == "smtp_failcmds" ) {
            if ( val < limit_smtp_failcmds ) return 0;
            emsg = limit_smtp_failcmds_emsg;
            return -1; 
        } else if ( limit_name == "connection_secs" ) {
            if ( val < limit_connection_secs ) return 0;
            emsg = limit_connection_secs_emsg;
            return -1; 
        } else if ( limit_name == "smtp_data_size" ) {
            if ( val < limit_smtp_data_size ) return 0;
            emsg = limit_smtp_data_size_emsg;
            return -1; 
        } else if ( limit_name == "smtp_rcpt_to" ) {
            if ( val < limit_smtp_rcpt_to ) return 0;
            emsg = limit_smtp_rcpt_to_emsg;
            return -1; 
        }
        // Shouldn't happen -- if we get here, there's an error in the source code!
        emsg = "500 Program config error";
        return -1;
    }

    // See if 'regex' matches remote hostname/ip 's'
    // Returns:
    //     1 -- match
    //     0 -- no match
    //
    int IsMatch(const char *regex, const char *s) {
        if ( RegexMatch(regex, s) == 1 ) {
            ISLOG("r") Log("DEBUG: Checking '%s' ~= '%s': Matched!\n", s, regex);
            return 1;   // match
        }
        ISLOG("r") Log("DEBUG: Checking '%s' ~= '%s': no\n", s, regex);
        return 0;
    }

    // See if remote host/ip allowed by specified regex
    // Returns:
    //     1 -- Remote is allowed
    //     0 -- Remote is NOT allowed
    //
    int IsRemoteAllowed(const char *regex) {
        if ( IsMatch(regex, G_remotehost) ) return 1;    // match? allowed
        if ( IsMatch(regex, G_remoteip  ) ) return 1;    // match? allowed
        return 0; // no match? not allowed
    }

    // See if remote allowed by global allow
    // Returns:
    //     1 -- Remote is allowed
    //     0 -- Remote is NOT allowed
    //
    int IsRemoteAllowed() {
        // Nothing configured? Allow anyone
        if ( allow_remotehost_regex.size() == 0 ) {
            ISLOG("w") Log("WARNING: All remotes allowed by default\n");
            return 1;
        } else {
            // If one or both configured, must have at least one match
            for ( size_t t=0; t<allow_remotehost_regex.size(); t++ )
                if ( IsRemoteAllowed(allow_remotehost_regex[t].c_str() ) )
                    return 1;   // match? allowed
            return 0;           // no match? not allowed
        }
    }

    // See if remote host is allowed by group.
    // Returns:
    //    1 -- Remote host is allowed by the group
    //    0 -- Remote host is not allowed
    //
    int IsRemoteAllowedByGroup(const string& groupname) {
        if ( groupname == "*" ) return 1;                       // '*' means always allow
        for ( size_t t=0; t<allowgroups.size(); t++ ) {         // find the group..
            AllowGroup &ag = allowgroups[t];
            if ( ag.name != groupname ) continue;               // no match, keep looking
            for ( size_t i=0; i<ag.regexes.size(); i++ )        // found group, check remote against all regexes in group
                if ( IsRemoteAllowed(ag.regexes[i].c_str()) )   // check remote hostname/ip
                    return 1;   // match found!
            return 0;           // no match; not allowed
        }
        // Didn't find allowgroup -- admin config error!
        Log("ERROR: group '%s' is referenced but not defined (fix your mailrecv.conf!)\n",
            groupname.c_str());
        return 0;
    }

    // Add allow group definition
    //    If name exists, add regex to that allowgroup.
    //    If name doesn't exist, add a new AllowGroup with that name+regex
    //
    // Returns:
    //     0 on success
    //    -1 on error (emsg has reason)
    //
    int AddAllowGroup(const char *name, const char *regex, string& emsg) {
        // Make sure regex compiles..
        if ( RegexMatch(regex, "x") == -1 ) {
            emsg = string("'") + string(regex) + "': bad perl regular expression";
            return -1;
        }
        // See if group name exists. If so, append regex, done.
        for ( size_t i=0; i<allowgroups.size(); i++ ) {
            AllowGroup &agroup = allowgroups[i];
            if ( agroup.name == name ) {
                agroup.regexes.push_back(regex); // append to existing
                return 0;                        // done
            }
        }
        // Not found? Create new..
        AllowGroup agroup;
        agroup.name = name;
        agroup.regexes.push_back(regex);
        allowgroups.push_back(agroup);
        return 0;
    }

    // See if allowgroup is defined
    int IsAllowGroupDefined(const char *groupname) {
        for ( size_t i=0; i<allowgroups.size(); i++ )
            if ( allowgroups[i].name == groupname )
                return 1; // yep
        return -1;  // nope
    }

    // Load the specified config file
    //     Returns 0 on success, -1 on error (reason printed on stderr)
    //
    int Load(const char *conffile) {
        int err = 0;
        FILE *fp;
        ISLOG("fc") Log("DEBUG: fopen(%s,'r')..\n", conffile);
        if ( (fp = fopen(conffile, "r")) == NULL) {
            Log("ERROR: can't open %s: %m\n", conffile);
            return -1;
        }
        char line[LINE_LEN+1], arg1[LINE_LEN+1], arg2[LINE_LEN+1], arg3[LINE_LEN+1];
        int linenum = 0;
        while ( fgets(line, LINE_LEN, fp) != NULL ) {
            // Keep count of lines
            ++linenum;

            // Strip comments, but keep trailing \n
            char *p = strchr(line,'#');
            if ( p ) { *p = 0; strcat(line, "\n"); }

            // Skip blank lines
            if ( line[0] == '\n' ) continue;

            // Show each line loaded if debugging..
            ISLOG("c") Log("DEBUG: Loading config: %s", line);   // line includes \n

            // Handle config commands..
            //
            //     Note: Our combo of fgets() and sscanf() with just %s is safe from overruns;
            //     line[] is limited to LINE_LEN by fgets(), so arg1/arg2 must be shorter.
            //
            if ( sscanf(line, "domain %s", arg1) == 1 ) {           // %s safe from overruns -- see Note above
                domain = arg1;
            } else if ( sscanf(line, "debug %s", arg1) == 1 ) {     // %s safe from overruns -- see Note above
                if ( !G_debugflags[0] ) {                           // no command line override?
                    if ( strcmp(arg1, "-") != 0 ) {
                        G_debugflags = (const char*)strdup(arg1);
                    }
                }
            } else if ( sscanf(line, "loghex %s", arg1) == 1 ) {    // %s safe from overruns -- see Note above
                int onoff = OnOff(arg1);
                if ( onoff < 0 ) {      // error?
                    Log("ERROR: '%s' (LINE %d): 'loghex %s' expected (on|off)\n",
                        conffile, linenum, arg1);
                    err = -1;
                    continue;
                }
                loghex = onoff;
            } else if ( sscanf(line, "logfile %s", arg1) == 1 ) {   // %s safe from overruns -- see Note above
                if ( G_logfilename == 0 ) {                         // no command line override?
                    if ( strcmp(arg1, "syslog") == 0 ) {
                        G_logfilename = 0;
                        G_logfp = 0;
                    } else {
                        G_logfilename = strdup(arg1);
                    }
                }
            } else if ( sscanf(line, "limit.smtp_commands %s %[^\n]", arg1, arg2) == 2 ) { // %s safe -- see Note above
                if ( sscanf(arg1, "%ld", &limit_smtp_commands) != 1 ) {
                    Log("ERROR: '%s' (LINE %d): '%s' not an integer\n", conffile, linenum, arg1);
                    err = -1;
                    continue;
                }
                limit_smtp_commands_emsg = arg2;
            } else if ( sscanf(line, "limit.smtp_unknowncmd %s %[^\n]", arg1, arg2) == 2 ) {
                if ( sscanf(arg1, "%ld", &limit_smtp_unknowncmd) != 1 ) {
                    Log("ERROR: '%s' (LINE %d): '%s' not an integer\n", conffile, linenum, arg1);
                    err = -1;
                    continue;
                }
                limit_smtp_unknowncmd_emsg = arg2;
            } else if ( sscanf(line, "limit.smtp_failcmds %s %[^\n]", arg1, arg2) == 2 ) {
                if ( sscanf(arg1, "%ld", &limit_smtp_failcmds) != 1 ) {
                    Log("ERROR: '%s' (LINE %d): '%s' not an integer\n", conffile, linenum, arg1);
                    err = -1;
                    continue;
                }
                limit_smtp_failcmds_emsg = arg2;
            } else if ( sscanf(line, "limit.connection_secs %s %[^\n]", arg1, arg2) == 2 ) {
                if ( sscanf(arg1, "%ld", &limit_connection_secs) != 1 ) {
                    Log("ERROR: '%s' (LINE %d): '%s' not an integer\n", conffile, linenum, arg1);
                    err = -1;
                    continue;
                }
                limit_connection_secs_emsg = arg2;
            } else if ( sscanf(line, "limit.smtp_data_size %s %[^\n]", arg1, arg2) == 2 ) {
                if ( sscanf(arg1, "%ld", &limit_smtp_data_size) != 1 ) {
                    Log("ERROR: '%s' (LINE %d): '%s' not an integer\n", conffile, linenum, arg1);
                    err = -1;
                    continue;
                }
                limit_smtp_data_size_emsg = arg2;
            } else if ( sscanf(line, "limit.smtp_rcpt_to %s %[^\n]", arg1, arg2) == 2 ) {
                if ( sscanf(arg1, "%ld", &limit_smtp_rcpt_to) != 1 ) {
                    Log("ERROR: '%s' (LINE %d): '%s' not an integer\n", conffile, linenum, arg1);
                    err = -1;
                    continue;
                }
                limit_smtp_rcpt_to_emsg = arg2;
            } else if ( sscanf(line, "limit.smtp_ascii %s %[^\n]", arg1, arg2) == 2 ) {
                int onoff = OnOff(arg1);
                if ( onoff < 0 ) {      // error?
                    Log("ERROR: '%s' (LINE %d): 'limit.smtp_ascii %s' expected (on|off)\n",
                        conffile, linenum, arg1);
                    err = -1;
                    continue;
                }
                limit_smtp_ascii      = onoff;
                limit_smtp_ascii_emsg = arg2;
            } else if ( sscanf(line, "deadletter_file %s", arg1) == 1 ) {
                deadletter_file = arg1;
            } else if ( sscanf(line, "allowgroup %s %s", arg1, arg2) == 2 ) {
                string emsg;
                if ( AddAllowGroup(arg1, arg2, emsg) < 0 ) {
                    Log("ERROR: '%s' (LINE %d): %s\n", conffile, linenum, emsg.c_str());
                    err = -1;
                    continue;
                }
            } else if ( sscanf(line, "deliver rcpt_to %s append %s", arg1, arg2) == 2 ) {
                deliver_rcpt_to_file_allowgroups.push_back("*");
                deliver_rcpt_to_file_address.push_back(arg1);
                deliver_rcpt_to_file_filename.push_back(arg2);
            } else if ( sscanf(line, "deliver allowgroup %s rcpt_to %s append %s", arg1, arg2, arg3) == 3 ) {
                if ( IsAllowGroupDefined(arg1) < 0 ) {
                    Log("ERROR: '%s' (LINE %d): allowgroup '%s' is undefined\n", conffile, linenum, arg1);
                    err = -1;
                    continue;
                }
                deliver_rcpt_to_file_allowgroups.push_back(arg1);
                deliver_rcpt_to_file_address.push_back(arg2);
                deliver_rcpt_to_file_filename.push_back(arg3);
            } else if ( sscanf(line, "deliver rcpt_to %s pipe %[^\n]", arg1, arg2) == 2 ) {
                deliver_rcpt_to_pipe_allowgroups.push_back("*");
                deliver_rcpt_to_pipe_address.push_back(arg1);
                deliver_rcpt_to_pipe_command.push_back(arg2);
            } else if ( sscanf(line, "deliver allowgroup %s rcpt_to %s pipe %[^\n]", arg1, arg2, arg3) == 3 ) {
                if ( IsAllowGroupDefined(arg1) < 0 ) {
                    Log("ERROR: '%s' (LINE %d): allowgroup '%s' is undefined\n", conffile, linenum, arg1);
                    err = -1;
                    continue;
                }
                deliver_rcpt_to_pipe_allowgroups.push_back(arg1);
                deliver_rcpt_to_pipe_address.push_back(arg2);
                deliver_rcpt_to_pipe_command.push_back(arg3);
            } else if ( sscanf(line, "error rcpt_to %s %[^\n]", arg1, arg2) == 2 ) {
                int ecode;
                // Make sure error message includes 3 digit SMTP error code
                if ( sscanf(arg2, "%d", &ecode) != 1 ) {
                    Log("ERROR: '%s' (LINE %d): missing 3 digit SMTP error message '%s'\n", conffile, linenum, arg2);
                    err = -1;
                    continue;
                }
                if ( RegexMatch(arg1, "x") == -1 ) { // Make sure regex compiles..
                    Log("ERROR: '%s' (LINE %d): bad 'error rcpt_to' regex '%s'\n", conffile, linenum, arg1);
                    err = -1;
                    continue;
                }
                errors_rcpt_to_regex.push_back(arg1);
                errors_rcpt_to_message.push_back(arg2);
            } else if ( sscanf(line, "replace rcpt_to %s %s", arg1, arg2) == 2 ) {
                // Make sure regex compiles..
                if ( RegexMatch(arg1, "x") == -1 ) {
                    Log("ERROR: '%s' (LINE %d): bad 'replace rcpt_to' regex '%s'\n", conffile, linenum, arg1);
                    err = -1;
                }
                replace_rcpt_to_regex.push_back(arg1);
                replace_rcpt_to_after.push_back(arg2);
            } else if ( sscanf(line, "allow remotehost %s", arg1) == 1 ) {
                // Make sure regex compiles..
                if ( RegexMatch(arg1, "x") == -1 ) {
                    Log("ERROR: '%s' (LINE %d): bad 'allow remotehost' regex '%s'\n", conffile, linenum, arg1);
                    err = -1;
                }
                allow_remotehost_regex.push_back(arg1);
            } else {
                Log("ERROR: '%s' (LINE %d): ignoring unknown config command: %s\n", conffile, linenum, line);
                err = -1;
            }
        }
        int ret = fclose(fp);
        ISLOG("f") Log("DEBUG: fclose() returned %d\n", ret);

        // Show everything we actually loaded..
        ISLOG("c") {
            Log("DEBUG: --- Config file:\n");
            Log("DEBUG:    debug: %s\n", G_debugflags);
            Log("DEBUG:    logfile: '%s'\n", G_logfilename);
            Log("DEBUG:    loghex: %d\n", LogHex());
            Log("DEBUG:    domain: '%s'\n", Domain());
            Log("DEBUG:    deadletter_file: '%s'\n", DeadLetterFile());
            Log("DEBUG:    limit_smtp_commands    max=%ld msg=%s\n", limit_smtp_commands,   limit_smtp_commands_emsg.c_str());
            Log("DEBUG:    limit_smtp_unknowncmd  max=%ld msg=%s\n", limit_smtp_unknowncmd, limit_smtp_unknowncmd_emsg.c_str());
            Log("DEBUG:    limit_smtp_failcmds    max=%ld msg=%s\n", limit_smtp_failcmds,   limit_smtp_failcmds_emsg.c_str());
            Log("DEBUG:    limit_connection_secs  max=%ld msg=%s\n", limit_connection_secs, limit_connection_secs_emsg.c_str());
            Log("DEBUG:    limit_smtp_data_size   max=%ld msg=%s\n", limit_smtp_data_size,  limit_smtp_data_size_emsg.c_str());
            Log("DEBUG:    limit_smtp_rcpt_to     max=%ld msg=%s\n", limit_smtp_rcpt_to,    limit_smtp_rcpt_to_emsg.c_str());
            Log("DEBUG:    limit_smtp_ascii       val=%d msg=%s\n",  limit_smtp_ascii,      limit_smtp_ascii_emsg.c_str());

            size_t t;
            // Allowgroups..
            for ( t=0; t<allowgroups.size(); t++ ) {
                ostringstream os;
                AllowGroup &ag = allowgroups[t];
                os << "DEBUG:    allowgroup '" << ag.name << "': ";
                for ( size_t i=0; i<ag.regexes.size(); i++ )
                    { os << (i>0?", ":"") << "'" << ag.regexes[i] << "'"; }
                Log("%s\n", os.str().c_str());
            }
            // deliver to file..
            for ( t=0; t<deliver_rcpt_to_file_address.size(); t++ ) {
                Log("DEBUG:    deliver rcpt_to: allowgroup='%s' address='%s', which writes to file='%s'\n",
                    deliver_rcpt_to_file_allowgroups[t].c_str(),
                    deliver_rcpt_to_file_address[t].c_str(),
                    deliver_rcpt_to_file_filename[t].c_str());
            }
            // deliver to pipe..
            for ( t=0; t<deliver_rcpt_to_pipe_address.size(); t++ ) {
                Log("DEBUG:    deliver rcpt_to: allowgroup='%s' address='%s', which pipes to cmd='%s'\n",
                    deliver_rcpt_to_pipe_allowgroups[t].c_str(),
                    deliver_rcpt_to_pipe_address[t].c_str(),
                    deliver_rcpt_to_pipe_command[t].c_str());
            }
            // global allow remotes..
            for ( t=0; t<allow_remotehost_regex.size(); t++ ) {
                Log("DEBUG:    allow remote hostnames that match perl regex '%s'\n",
                    allow_remotehost_regex[t].c_str());
            }
            Log("DEBUG: ---\n");
        }
        return err;     // let caller decide what to do
    }

    // Deliver mail to recipient.
    //     If there's no configured recipient, write to deadletter file.
    //
    // Returns:
    //     0 on success
    //    -1 on error (reason sent to server on stdout).
    //
    int DeliverMail(const char* mail_from,          // SMTP 'mail from:'
                    const char *rcpt_to,            // SMTP 'rcpt to:'
                    const vector<string>& letter) { // email contents, including headers, blank line, body
        size_t t;

        // Check for 'append to file' recipient..
        for ( t=0; t<deliver_rcpt_to_file_address.size(); t++ ) {
            const string& groupname = deliver_rcpt_to_file_allowgroups[t];
            if ( strcmp(rcpt_to, deliver_rcpt_to_file_address[t].c_str()) == 0 ) {
                if ( IsRemoteAllowedByGroup(groupname) ) {
                    // TODO: Check error return of AppendMailToFile(), fall thru to deadletter?
                    AppendMailToFile(mail_from, rcpt_to, letter, deliver_rcpt_to_file_filename[t]);
                    ISLOG("+") Log("Mail from=%s to=%s [append to '%s']\n", 
                         mail_from, rcpt_to, deliver_rcpt_to_file_filename[t].c_str());
                    return 0;   // delivered
                }
                Log("'%s': remote server %s [%s] not allowed to send to this address\n",
                    rcpt_to, G_remotehost, G_remoteip);
                SMTP_Reply("550 Server not allowed to send to this address");
                return -1;
            }
        }

        // Check for 'pipe to command' recipient..
        for ( t=0; t<deliver_rcpt_to_pipe_address.size(); t++ ) {
            const string& groupname = deliver_rcpt_to_pipe_allowgroups[t];
            if ( strcmp(rcpt_to, deliver_rcpt_to_pipe_address[t].c_str()) == 0 ) {
                // Check allowgroup ('*' matches everything)
                if ( IsRemoteAllowedByGroup(groupname) ) {
                    // TODO: Check error return of PipeMailToCommand(), fall thru to deadletter?
                    PipeMailToCommand(mail_from, rcpt_to, letter, deliver_rcpt_to_pipe_command[t]);
                    ISLOG("+") Log("Mail from=%s to=%s [pipe to '%s']\n", 
                             mail_from, rcpt_to, deliver_rcpt_to_pipe_command[t].c_str());
                    return 0;   // delivered
                }
                Log("'%s': remote server %s [%s] not allowed to send to this address\n",
                    rcpt_to, G_remotehost, G_remoteip);
                SMTP_Reply("550 Server not allowed to send to this address");
                return -1;
            }
        }

        // If we're here, nothing matched.. write to deadletter file
        // TODO: Pass back actual OS error to remote as part of SMTP response
        //
        if ( AppendMailToFile(mail_from, rcpt_to, letter, deadletter_file) < 0 )
            return -1;    // failed deadletter delivery? Tell remote we can't deliver

        ISLOG("+") Log("Mail from=%s to=%s [append to deadletter file '%s']\n",
            mail_from, rcpt_to, deadletter_file.c_str());
        return 0;   // delivered
    }

    // See if address is an error address, or if server not allowed
    //
    // Returns:
    //    0 -- OK to deliver -- not an error address
    //   -1 -- Reject delivery -- send error message in 'emsg' to remote
    //
    int CheckErrorAddress(const char *address, string& emsg) {
        size_t t;
        // First, ignore address configured for regular delivery..
        // ..rcpt_to file?
        for ( t=0; t<deliver_rcpt_to_file_address.size(); t++ ) {
            if ( strcmp(address, deliver_rcpt_to_file_address[t].c_str()) == 0 ) {
                if ( IsRemoteAllowedByGroup(deliver_rcpt_to_file_allowgroups[t].c_str()) ) {
                    return 0;         // OK to deliver
                } else {
                    emsg = "550 Remote not configured to deliver for this address";
                    return -1;
                }
            }
        }
        // ..rcpt_to pipe?
        for ( t=0; t<deliver_rcpt_to_pipe_address.size(); t++ ) {
            if ( strcmp(address, deliver_rcpt_to_pipe_address[t].c_str()) == 0 ) {
                if ( IsRemoteAllowedByGroup(deliver_rcpt_to_pipe_allowgroups[t].c_str()) ) {
                    return 0;         // OK to deliver
                } else {
                    emsg = "550 Remote not configured to deliver for this address";
                    return -1;
                }
            }
        }
        // Check error addresses last
        for ( t=0; t<errors_rcpt_to_regex.size(); t++ ) {
            if ( RegexMatch(errors_rcpt_to_regex[t].c_str(), address) == 1 ) { // reject address configured?
                emsg = errors_rcpt_to_message[t];
                emsg += " '";
                emsg += address;
                emsg += "'";
                return -1;  // return error msg
            }
        }
        return 0;           // OK to deliver
    }

    // Child thread for execution timer
    static void *ChildExecutionTimer(void *data) {
        Configure *self = (Configure*)data; // no lock needed; conf loaded before timer started
        string emsg = self->limit_connection_secs_emsg; emsg += "\n";
        long secs = long(self->limit_connection_secs);
        sleep(secs);                        // use sleep() for timer
        // Log to stderr from child thread
        //    Write to log first -- write() to remote may hang if fail2ban banned it
        Log("%s", emsg.c_str());
        // Timer expired? Send message to remote and exit immediately
        write(1, emsg.c_str(), strlen(emsg.c_str()));
        exit(0);
    }

    // Start execution timer thread
    //    Use thread (instead of fork()) so child reports proper PID in log msgs.
    //
    void StartExecutionTimer() {
        static pthread_t dataready_tid = 0;
        if ( dataready_tid != 0 ) return;   // only run once
        pthread_create(&dataready_tid, NULL, ChildExecutionTimer, (void*)this);
    }
};

Configure G_conf;

// Minimum commands we must support:
//      HELO MAIL RCPT DATA RSET NOOP QUIT VRFY

// Return with remote's ip address + hostname in globals
//    Sets globals: G_remotehost, G_remoteip
//
//    fd -- tcp connection file descriptor (typically stdin because xinetd invoked us)
//
// Returns:
//     0 -- success (got IP for sure, may or may not have gotten remote hostname)
//    -1 -- could not determine any remote info
//
int GetRemoteHostInfo(int fd) {
    strcpy(G_remoteip, "?.?.?.?");
    strcpy(G_remotehost, "???");

    struct sockaddr_storage ss;
    socklen_t ss_size = sizeof(struct sockaddr_storage);         // allows for ipv6

    // Get remote's sockaddr based on fd
    if (getpeername(fd, (struct sockaddr*)&ss, &ss_size)<0) {
        // fail
        Log("getpeername(): couldn't determine remote IP address: %s\n", strerror(errno));
        return -1;
    }

    // Get remote's numeric ip as string
    int gaierr;
    if ((gaierr = getnameinfo((struct sockaddr*)&ss, ss_size,
                             G_remoteip, sizeof(G_remoteip),     // remote ip string
                             NULL, 0,                            // remote port string (ignore)
                             NI_NUMERICHOST))!=0) {              // numeric IP required
        // fail
        Log("getnameinfo(NumericHost|NumericPort): %s\n", gai_strerror(gaierr));
        return -1;
    }

    // Get remote's hostname as string
    if ((gaierr = getnameinfo((struct sockaddr*)&ss, ss_size,
                             G_remotehost, sizeof(G_remotehost), // remote hostname string
                             NULL, 0,                            // remote port string (ignore)
                             NI_NAMEREQD))!=0) {                 // hostname needed
        // fail
        Log("getnameinfo(NameReqd): %s\n", gai_strerror(gaierr));
        return -1;
    }

    return 0;
}

// Truncate string at first CR|LF
void StripCRLF(char *s) {
    char *eol;
    if ( (eol = strchr(s, '\r')) ) { *eol = 0; }
    if ( (eol = strchr(s, '\n')) ) { *eol = 0; }
}

#define ISCMD(x)        !strcasecmp(cmd, x)
#define ISARG1(x)       !strcasecmp(arg1, x)

// Read SMTP DATA letter contents from remote
//     Assumes an SMTP "DATA" command was just received.
//
//     TODO: Should insert a "Received:" block into the headers, above first.
//     TODO:    Received: from <HELO_FROM> (remotehost [remoteIP])
//     TODO:              by ourdomain.com (mailrecv) with SMTP id ?????
//     TODO:              for <rcpt_to>; Sat,  8 Sep 2018 23:44:11 -0400 (EDT)
//
// Returns:
//     0 on success
//    -1 general failure (premature end of input, limit reached)
//       emsg has error to send remote.
//
int SMTP_ReadLetter(FILE *fp,                    // [in] connection to remote
                    vector<string>& letter,      // [in] array for saved letter
                    string &emsg) {              // [out] error to send remote on return -1
    char s[LINE_LEN+1];
    long bytecount = 0;
    while (fgets(s, LINE_LEN, fp)) {
        // Remove trailing CRLF
        StripCRLF(s);
        ISLOG("l") Log("DEBUG: Letter: '%s'\n", s);
        // End of letter? done
        if ( strcmp(s, ".") == 0 ) return 0;    // <CRLF>.<CRLF>
        // Check limit
        bytecount += strlen(s);
        if ( G_conf.CheckLimit(bytecount, "smtp_data_size", emsg) < 0 ) {
            Log("SMTP DATA limit reached (%ld)\n", bytecount);
            return -1;
        }
        // Otherwise append lines with CRLF removed to letter
        if ( s[0] == '.' ) letter.push_back(s+1);   // RFC 822 4.5.2 'Transparency'
        else               letter.push_back(s);
    }
    // Unexpected end of input
    Log("Premature end of input while receiving email from remote\n");
    emsg = "550 End of input during DATA command";
    return -1;                  // premature end of input
}

// Handle a complete SMTP session with the remote on stdin/stdout
// Returns main() exit code:
//     0 -- success
//     1 -- failure
//
// The following are the SMTP commands: (RFC 821 4.1.2)
//
//            COMMAND ARGUMENT                            MAILRECV?
//            ------- ---------------------------------   ---------
//            HELO <SP> <domain> <CRLF>                   YES
//            MAIL <SP> FROM:<reverse-path> <CRLF>        YES
//            RCPT <SP> TO:<forward-path> <CRLF>          YES
//            DATA <CRLF>                                 YES
//            RSET <CRLF>                                 YES
//            SEND <SP> FROM:<reverse-path> <CRLF>        no
//            SOML <SP> FROM:<reverse-path> <CRLF>        no
//            SAML <SP> FROM:<reverse-path> <CRLF>        no
//            VRFY <SP> <string> <CRLF>                   YES
//            EXPN <SP> <string> <CRLF>                   no
//            HELP [<SP> <string>] <CRLF>                 YES
//            NOOP <CRLF>                                 YES
//            QUIT <CRLF>                                 YES
//            TURN <CRLF>                                 no
//
int HandleSMTP() {
    vector<string> letter;              // array for received email (SMTP "DATA")
    char line[LINE_LEN+1],              // raw line buffer
         cmd[LINE_LEN+1],               // cmd received
         arg1[LINE_LEN+1],              // arg1 received
         arg2[LINE_LEN+1],              // arg2 received
         mail_from[LINE_LEN+1] = "";    // The remote's "MAIL FROM:" value
    vector<string> rcpt_tos;            // The remote's "RCPT TO:" value(s)
    const char *our_domain = G_conf.Domain();

    // We implement RFC 821 "HELO" protocol only.. no fancy EHLO stuff.
    {
        ostringstream os;
        os << "220 " << our_domain << " SMTP (RFC 821/822)";    // TODO -- allow custom identity to be specified
        SMTP_Reply(os.str().c_str());
    }

    // Limit counters
    int smtp_commands_count = 0;
    int smtp_unknowncmd_count = 0;
    int smtp_fail_commands_count = 0;
    int smtp_rcpt_to_count = 0;
    long smtp_cmd_flags = 0;       // bit field, one bit for each SMTP command received
    // CheckLimit() returned error msg, if any
    string emsg;
    int quit = 0;
    // READ ALL SMTP COMMANDS FROM REMOTE UNTIL "QUIT" OR EOF
    while (!quit && fgets(line, LINE_LEN-1, stdin)) {
        line[LINE_LEN] = 0;        // extra caution
        StripCRLF(line);

        // LOG THE RECEIVED SMTP COMMAND
        ISLOG("s") {
            if ( G_conf.LogHex() ) {
                // Handle if we should log any binary from the remote as hex
                char *line_safe = AsciiHexEncode(line);
                Log("DEBUG: SMTP cmd: %s\n", line_safe);
                free(line_safe);
            } else {
                Log("DEBUG: SMTP cmd: %s\n", line);
            }
            Log("DEBUG: SMTP cmd: cmdcount=%d, unknowncount=%d, failcount=%d\n",
                smtp_commands_count, smtp_unknowncmd_count, smtp_fail_commands_count);
        }

        // LIMIT CHECK: # SMTP COMMANDS
        //    NOTE: Empty lines count towards the command counter..
        //
        if ( G_conf.CheckLimit(++smtp_commands_count, "smtp_commands", emsg) < 0 ) {
            Log("SMTP #commands limit reached (%d)\n", smtp_commands_count);
            SMTP_Reply(emsg.c_str());
            break;      // end session
        }

        // WAS THERE BINARY DATA IN SMTP COMMAND THAT IS NOT ALLOWED?
        //    Do this check AFTER limit check for command count
        //
        if ( BinaryCheck(line) && G_conf.LimitSmtpAscii() ) {
            ++smtp_fail_commands_count;
            SMTP_Reply("500 Binary data (non-ASCII) unsupported");
            goto command_done;
        }

        // Break up command into args
        //    note: fgets() already ensures LINE_LEN max, so
        //          sscanf() does not need to re-enforce length max.
        //
        arg1[0] = arg2[0] = 0;
        if ( sscanf(line, "%s%s%s", cmd, arg1, arg2) < 1 ) continue;
        arg1[LINE_LEN] = 0;     // extra caution
        arg2[LINE_LEN] = 0;

        if ( ISCMD("QUIT") ) {
            smtp_cmd_flags |= SMTP_CMD_QUIT;
            quit = 1;
            ostringstream os;
            os << "221 " << our_domain << " closing connection";
            SMTP_Reply(os.str().c_str());
        } else if ( ISCMD("HELO") ) {
            smtp_cmd_flags |= SMTP_CMD_HELO;
            ostringstream os;
            os << "250 " << our_domain << " Hello " << G_remotehost << " [" << G_remoteip << "]";
            SMTP_Reply(os.str().c_str());
        } else if ( ISCMD("MAIL") ) {
            smtp_cmd_flags |= SMTP_CMD_MAIL;
            if ( ISARG1("FROM:")) {                         // "MAIL FROM: foo@bar.com"? (space after ":")
                strcpy(mail_from, arg2);
                ostringstream os;
                os << "250 '" << mail_from << "': Sender ok";
                SMTP_Reply(os.str().c_str());
            } else {
                if ( strncasecmp(arg1,"FROM:", 5) == 0 ) {  // "MAIL FROM:foo@bar.com"? (NO space after ":")
                    strcpy(mail_from, arg1+5);              // get address after the ":"
                    ostringstream os;
                    os << "250 '" << mail_from << "': Sender ok";
                    SMTP_Reply(os.str().c_str());
                } else {
                    ++smtp_fail_commands_count;
                    ostringstream os;
                    os << "501 Unknown argument '" << arg1 << "'";
                    SMTP_Reply(os.str().c_str());
                    Log("ERROR: unknown MAIL argument '%s'\n", arg1);
                }
            }
        } else if ( ISCMD("RCPT") ) {
            smtp_cmd_flags |= SMTP_CMD_RCPT;
            // RFC 5321 3.3:
            //     "If RCPT appears w/out previous MAIL, server MUST return a
            //      503 "Bad sequence of commands" response."
            //
            if ( mail_from[0] == 0 ) {
                ++smtp_fail_commands_count;
                SMTP_Reply("503 Bad sequence of commands -- missing MAIL FROM");
                Log("ERROR: 'RCPT' before 'MAIL': bad sequence of commands\n", arg1);
                goto command_done;
            }
            char *address;
            if ( ISARG1("TO:") ) {                             // "RCPT TO: foo@bar.com" (not recommended RFC 5321)
                address = arg2;
                goto rcpt_to;
            } else if ( strncasecmp(arg1, "TO:", 3) == 0 ) {   // "RCPT TO:foo@bar.com"? (NO space after ":")
                address = arg1 + 3;                            // get address after the ":"
rcpt_to:
                IsolateAddress(address);                       // "<foo@bar.com>" -> "foo@bar.com"

                // LIMIT CHECK: # RCPT TO COMMANDS
                if ( G_conf.CheckLimit(++smtp_rcpt_to_count, "smtp_rcpt_to", emsg) < 0 ) {
                    ++smtp_fail_commands_count;
                    Log("SMTP Number of 'rcpt to' recipients limit reached (%d)\n", smtp_rcpt_to_count);
                    SMTP_Reply(emsg.c_str());
                    // Fail2ban parseable error
                    ISLOG("F") Log("ERROR: [%s] %s\n", G_remoteip, emsg.c_str());
                    break;  // end session
                }
                if ( G_conf.CheckErrorAddress(address, emsg) < 0 ) {
                    ++smtp_fail_commands_count;
                    SMTP_Reply(emsg.c_str());                  // Failed: send error, don't deliver
                    // Fail2ban parseable error
                    ISLOG("F") Log("ERROR: [%s] %s\n", G_remoteip, emsg.c_str());
                } else {
                    rcpt_tos.push_back(address);               // Passed: ok to deliver
                    ostringstream os;
                    os << "250 " << address << "... recipient ok";
                    SMTP_Reply(os.str().c_str());
                }
            } else {
                ++smtp_fail_commands_count;
                ostringstream os;
                os << "501 Unknown RCPT argument '" << arg1 << "'";
                SMTP_Reply(os.str().c_str());
                Log("ERROR: unknown RCPT argument '%s'\n", arg1);
            }
        } else if ( ISCMD("DATA") ) {
            smtp_cmd_flags |= SMTP_CMD_DATA;
            if ( rcpt_tos.size() == 0 ) {
                ++smtp_fail_commands_count;
                SMTP_Reply("503 Bad sequence of commands -- missing RCPT TO");
            } else if ( mail_from[0] == 0 ) {
                ++smtp_fail_commands_count;
                SMTP_Reply("503 Bad sequence of commands -- missing MAIL FROM");
            } else {
                SMTP_Reply("354 Start mail input; end with <CRLF>.<CRLF>");
                if ( SMTP_ReadLetter(stdin, letter, emsg) == -1 ) {
                    ++smtp_fail_commands_count;
                    SMTP_Reply(emsg.c_str());
                    break;              // break fgets() loop
                }
                if ( letter.size() < 3 ) {
                    // Even a one line email has more header lines than this
                    SMTP_Reply("554 Message data was too short");
                    ++smtp_fail_commands_count;
                } else {
                    // Handle mail delivery
                    for ( size_t t=0; t<rcpt_tos.size(); t++ ) {
                        const char *rcpt_to = rcpt_tos[t].c_str();
                        if ( G_conf.DeliverMail(mail_from, rcpt_to, letter) == 0 ) {
                            SMTP_Reply("250 Message accepted for delivery");
                        } else {
                            ++smtp_fail_commands_count;
                        }
                    }
                }
                // Clear these
                mail_from[0] = 0;
                rcpt_tos.clear();
                letter.clear();
            }
        } else if ( ISCMD("VRFY") ) {
            smtp_cmd_flags |= SMTP_CMD_VRFY;
            SMTP_Reply("252 send some mail, will try my best");
        } else if ( ISCMD("RSET") ) {
            smtp_cmd_flags |= SMTP_CMD_RSET;
            mail_from[0] = 0;
            rcpt_tos.clear();
            letter.clear();
            SMTP_Reply("250 OK");
        } else if ( ISCMD("NOOP") ) {
            smtp_cmd_flags |= SMTP_CMD_NOOP;
            SMTP_Reply("250 OK");
        } else if ( ISCMD("HELP") ) {
            smtp_cmd_flags |= SMTP_CMD_HELP;
            SMTP_Reply("214-Help:");
            SMTP_Reply("214-HELO, DATA, RSET, NOOP, QUIT,");
            SMTP_Reply("214-MAIL FROM:, RCPT TO:, VRFY, HELP,");
            SMTP_Reply("214 EXPN, SEND, SOML, SAML, TURN");
        } else if ( ISCMD("EXPN") ) {
            smtp_cmd_flags |= SMTP_CMD_EXPN;
no_support:
            ++smtp_fail_commands_count;
            SMTP_Reply("502 Command not implemented or disabled");
            Log("ERROR: Remote tried '%s', we don't support it\n", cmd);
        } else if ( ISCMD("SEND") ) { smtp_cmd_flags |= SMTP_CMD_SEND; goto no_support;
        } else if ( ISCMD("SOML") ) { smtp_cmd_flags |= SMTP_CMD_SOML; goto no_support;
        } else if ( ISCMD("SAML") ) { smtp_cmd_flags |= SMTP_CMD_SAML; goto no_support;
        } else if ( ISCMD("TURN") ) { smtp_cmd_flags |= SMTP_CMD_TURN; goto no_support;
        } else if ( ISCMD("EHLO") ) {
            smtp_cmd_flags |= SMTP_CMD_EHLO;
            // EHLO is commonly sent first by remotes.
            //      Log as "IGNORED" (instead of ERROR) so syslog doesn't highlight it in red.
            SMTP_Reply("500 Unknown command");
            Log("IGNORED: Remote tried '%s'\n", cmd);
        } else {
            ++smtp_fail_commands_count;
            SMTP_Reply("500 Unknown command");
            Log("ERROR: Remote tried '%s', unknown command\n", cmd);

            // LIMIT CHECK: # UNKNOWN SMTP COMMANDS
            if ( G_conf.CheckLimit(++smtp_unknowncmd_count, "smtp_unknowncmd", emsg) < 0 ) {
                Log("SMTP #unknown commands limit reached (%d)\n", smtp_unknowncmd_count);
                SMTP_Reply(emsg.c_str());
                break;  // end session
            }
        }

command_done:
        // All commands end up here, successful or not
        fflush(stdout);

        // LIMIT CHECK: # UNKNOWN SMTP COMMANDS
        if ( G_conf.CheckLimit(smtp_fail_commands_count, "smtp_failcmds", emsg) < 0 ) {
            Log("SMTP #failed commands limit reached (%d)\n", smtp_fail_commands_count);
            SMTP_Reply(emsg.c_str());
            break;  // end session
        }
    }

    // Flush any closing responses to remote
    fflush(stdout);

    // Log what commands were used
    ISLOG("F") Log("INFO: connection closed. smtp_cmd_flags=0x%04lx\n",
                   smtp_cmd_flags);

    if ( quit ) {
        // Normal end to session
        return 0;
    } else {
        // If we're here, connection closed with no "QUIT".
        ISLOG("w") Log("WARNING: Premature end of input for SMTP commands\n");
        return 1;               // indicate a possible network error occurred
    }
}

// Show help and exit
void HelpAndExit() {
    const char *helpmsg =
          "mailrecv - a simple SMTP xinetd daemon (V " VERSION ")\n"
          "           See LICENSE file packaged with mailrecv for license/copyright info.\n"
          "\n"
          "Options\n"
          "    -c config-file       -- Use 'config-file' instead of default (" CONFIG_FILE ")\n"
          "    -d <logflags|->      -- Enable debugging logging flags (overrides conf file 'debug').\n"
          "    -l syslog|<filename> -- Set logfile (overrides conf file 'logfile')\n"
          "\n"
          "<logflags>\n"
          "    Can be one or more of these single letter flags:\n"
          "        - -- disables all debug logging\n"
          "        a -- all (enables all optional flags)\n"
          "        c -- show config file loading process\n"
          "        s -- show SMTP commands remote sent us\n"
          "        l -- show email contents as it's received (SMTP 'DATA' command's input)\n"
          "        r -- show regex pattern match checks\n"
          "        f -- show all open/close operations on files/pipes\n"
          "        w -- log non-essential warnings\n"
          "        F -- fail2ban style error messages (that include IP on same line)\n"
          "        + -- logs MAIL FROM/TO commands\n"
          "\n"
          "Example:\n"
          "    mailrecv -d srF -c mailrecv-test.conf -l /var/log/mailrecv.log\n"
          "    mailrecv -d c -c mailrecv-test.conf -l /dev/tty     # log to your terminal\n"
          "\n";
    fprintf(stderr, "%s", helpmsg);
    exit(1);
}

int main(int argc, const char *argv[]) {

    // Force bourne shell for popen(command)..
    setenv("SHELL", "/bin/sh", 1);

    // Initial config file
    const char *conffile = CONFIG_FILE;

    // Parse command line, possibly override default conffile, etc.
    for (int t=1; t<argc; t++) {
        if (strcmp(argv[t], "-c") == 0) {
            if (++t >= argc) {
                Log("ERROR: expected filename after '-c'\n");
                return 1;
            }
            conffile = argv[t];
        } else if (strcmp(argv[t], "-d") == 0) {
            if (++t >= argc) {
                G_debugflags = "a";
            } else {
                if ( strcmp(argv[t], "-") == 0 ) {
                    G_debugflags = "";
                } else {
                    G_debugflags = argv[t];
                }
            }
        } else if (strcmp(argv[t], "-l") == 0) {
            if (++t >= argc) {
                Log("ERROR: expected syslog|filename after '-l'\n");
                return 1;
            }
            if ( strcmp(argv[t], "syslog") == 0 ) {
                G_logfilename = 0;
                G_logfp = 0;
            } else {
                G_logfilename = strdup(argv[t]);
            }
        } else if (strncmp(argv[t], "-h", 2) == 0) {
            HelpAndExit();
        } else {
            Log("ERROR: unknown argument '%s'\n", argv[t]);
            exit(1);
            // HelpAndExit();   // bad: shows up on remote
        }
    }

    // Load config file
    if ( G_conf.Load(conffile) < 0 ) {
        // Tell remote we can't receive SMTP at this time
        SMTP_Reply("221 Cannot receive messages at this time.");
        Log("SMTP connection from remote host %s [%s]\n", G_remotehost, G_remoteip);
        Log("ERROR: '%s' has errors (above): told remote 'Cannot receive email at this time'\n", conffile);
        return 1;       // fail
    }

    // Do this AFTER loading config, so we can Log() errors properly..
    GetRemoteHostInfo(fileno(stdin));

    // Log remote host connection AFTER config loaded
    //     ..in case config sets 'logfile'
    //
    if ( G_logfilename ) Log("\n");     // RFE: albrecht 03/28/2020
    Log("SMTP connection from remote host %s [%s]\n", G_remotehost, G_remoteip);

    // Check if remote allowed to connect to us
    if ( ! G_conf.IsRemoteAllowed() ) {
        ostringstream os;
        os << "221 Cannot receive messages from " << G_remotehost
           << " [" << G_remoteip << "] at this time.";
        SMTP_Reply(os.str().c_str());
        Log("DENIED: Connection from %s [%s] not in allow_remotehost/ip lists\n", G_remotehost, G_remoteip);
        return 1;
    }

    // Start execution timer
    G_conf.StartExecutionTimer();

    // Handle the SMTP session with the remote
    return HandleSMTP();
}
