// vim: autoindent tabstop=8 shiftwidth=4 expandtab softtabstop=4

#include <stdio.h>
#include <string.h>

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

int main() {
    char s[80];
    strcpy(s, "a@b");               printf("BEFORE: '%s'\n", s); IsolateAddress(s); printf(" AFTER: '%s'\n\n", s);
    strcpy(s, "  <a@bcd>");         printf("BEFORE: '%s'\n", s); IsolateAddress(s); printf(" AFTER: '%s'\n\n", s);
    strcpy(s, "    ");              printf("BEFORE: '%s'\n", s); IsolateAddress(s); printf(" AFTER: '%s'\n\n", s);
    strcpy(s, "<foo@bar>");         printf("BEFORE: '%s'\n", s); IsolateAddress(s); printf(" AFTER: '%s'\n\n", s);
    strcpy(s, "   <<<foo@bar>");    printf("BEFORE: '%s'\n", s); IsolateAddress(s); printf(" AFTER: '%s'\n\n", s);
    strcpy(s, "<<<foo@bar>");       printf("BEFORE: '%s'\n", s); IsolateAddress(s); printf(" AFTER: '%s'\n\n", s);
    strcpy(s, "<foo@bar>>>");       printf("BEFORE: '%s'\n", s); IsolateAddress(s); printf(" AFTER: '%s'\n\n", s);
    strcpy(s, "Foo Bar <foo@bar>"); printf("BEFORE: '%s'\n", s); IsolateAddress(s); printf(" AFTER: '%s'\n\n", s);
    strcpy(s, "<f@b");              printf("BEFORE: '%s'\n", s); IsolateAddress(s); printf(" AFTER: '%s'\n\n", s);
    strcpy(s, "f@b>");              printf("BEFORE: '%s'\n", s); IsolateAddress(s); printf(" AFTER: '%s'\n\n", s);
    strcpy(s, "<>");                printf("BEFORE: '%s'\n", s); IsolateAddress(s); printf(" AFTER: '%s'\n\n", s);
    strcpy(s, "aaaaaaaaa");         printf("BEFORE: '%s'\n", s); IsolateAddress(s); printf(" AFTER: '%s'\n\n", s);
    strcpy(s, "  <>");              printf("BEFORE: '%s'\n", s); IsolateAddress(s); printf(" AFTER: '%s'\n\n", s);
    while ( fgets(s, 80, stdin) ) {
        s[strlen(s)-1] = 0;     // trim crlf
        printf("BEFORE: '%s'\n", s);
        IsolateAddress(s);
        printf(" AFTER: '%s'\n", s);
    }
    return 0;
}
