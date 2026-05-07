// vim: autoindent tabstop=8 shiftwidth=4 expandtab softtabstop=4

#include <stdio.h>
#include <ctype.h>      // toupper()
#include <time.h>       // time(), localtime()..
#include <string.h>     // strchr()
#include <errno.h>      // errno
#include <stdlib.h>     // exit()
#include <unistd.h>     // sleep(), gethostname()
#include <stdarg.h>     // vargs
#include <syslog.h>     // syslog()
#include <pcre.h>       // perl regex API (see 'man pcreapi(3)')
#include <sys/socket.h> // getpeername()
#include <netdb.h>      // gethostbyaddr(), NI_MAXHOST..
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/file.h>   // flock()
#include <pthread.h>    // pthread_create() for execution timer
// STL
#include <string>
#include <vector>
#include <sstream>

using namespace std;

char G_localhost[256];                   // local hostname
char G_remotehost[256] = "some_remote";  // Remote's hostname
char G_remoteip[NI_MAXHOST] = "1.2.3.4"; // Remote's IP address

// Return current time as localtime()
struct tm* GetLocaltime(void) {
    time_t secs;        // Current UNIX time
    time(&secs);
    return localtime(&secs);
}

// Return date as e.g. 'Fri, 24 Apr 2026 16:28:03 -0700 (PDT)'
//                      |    |  |   |    |  |  |   |      |
//                      %a   %d %b  %G   %H %M %S  %z     %Z
void GetRFCDate(char *datestr, int len) {
    strftime(datestr, len,
             "%a, %d %b %G %H:%M:%S %z (%Z)", GetLocaltime());
    datestr[0] = toupper(datestr[0]);  // upcase first letter of day abbrev
}

// Return date in current locale format, e.g. 'Thu May  7 08:54:15 PDT 2026'
void GetLogDate(char *datestr, int len) {
    // POSIX locale: "%a %b %e %H:%M:%S %Y" 
    strftime(datestr, len, "%c", GetLocaltime());
}

// Modify 'letter', inserting Return-Path:/Received: headers (RFC 821, 4.1.1 'DATA')
void AddReturnPath(vector<string>& letter, const char* mail_from) {
    char rfc_datestr[1024]; GetRFCDate(rfc_datestr, sizeof(rfc_datestr));
    string return_path = string("Return-Path: <") + string(mail_from)
                       + string(">");
    string received    = string("Received: from ") + string(G_remotehost)
                       + string(" by ") + string(G_localhost)
                       + string(" ; ") + string(rfc_datestr);
    letter.insert(letter.begin()+0, return_path);   // Return-Path: at top
    letter.insert(letter.begin()+1, received);      // Received: below Return-Path:
}

// Show letter on stdout
void ShowLetter(const char *msg, vector<string>& letter) {
    printf("%s\n", msg);
    for (int t=0; t<letter.size(); t++) {
        printf("%s\n", letter[t].c_str());
    }
}

// Return localhost in 'hostname', not to exceed 'size'
//     Errors sent to Log()
//
void GetLocalHostname(char *hostname, int len) {
    if (gethostname(hostname, len) < 0) {       // unistd
        printf("gethostname() failed: can't determine localhost name\n");
        strcpy(hostname, "LOCALHOST");
    }
    hostname[len-1] = 0;        // POSIX.1 re: truncation
}

int main() {

    GetLocalHostname(G_localhost, sizeof(G_localhost));

    vector<string> letter;
    // Test letter
    letter.push_back("From: joe@foo.com");
    letter.push_back("To: fred@bar.com");
    letter.push_back("Subject: something");
    letter.push_back("");
    letter.push_back("Line one");
    letter.push_back("Line two");
    ShowLetter("--- BEFORE:", letter);
    printf("<EOL>\n\n");

    // Add return path headers
    AddReturnPath(letter, "JOE@FOO.COM");
    ShowLetter("--- AFTER:", letter);
    printf("<EOL>\n\n");

    return 0;
}
