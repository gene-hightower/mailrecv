#include <pcre.h>	// perl regex library
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

//
// With reference to: https://www.mitchr.me/SS/exampleCode/AUPG/pcre_example.c.html
//    -erco 08/25/2018
//

int main() {
    const char *regex_errorstr;		// returned error if any
    int         regex_erroroff;		// returned error offset
    int         substrvec[30];		// pcre_exec()'s argument
    const char  *match;			// string to match
    int         ret;

    // Compile the regex..
    const char *regex = "^mail[0-9a-zA-Z-]*.example.com$";
    pcre *regex_compiled = pcre_compile(regex, 0, &regex_errorstr, &regex_erroroff, NULL);
    if ( regex_compiled == NULL ) {
        fprintf(stderr, "ERROR: could not compile '%s': %s\n", regex, regex_errorstr);
	return 1;
    }
    // Optimize regex
    pcre_extra *regex_extra = pcre_study(regex_compiled, 0, &regex_errorstr);
    if ( regex_errorstr != NULL ) {
        fprintf(stderr, "ERROR: Could not study '%s': %s\n", regex, regex_errorstr);
	return 1;
    }

    // Now see if we can match a string THAT SHOULD MATCH
    match = "mail-123-abc.example.com";
    printf("Match %s <-> %s? ", regex, match);
    ret = pcre_exec(regex_compiled, regex_extra, match, strlen(match), 0, PCRE_ANCHORED, substrvec, 30);
    if ( ret < 0 ) {
        // Error occurred
        printf("NO *UNEXPECTED* - ");
	switch (ret) {
	    case PCRE_ERROR_NOMATCH: printf("did not match\n"); break;
	    default: printf("bad regex or other error\n"); break;
	}
    } else {
        printf("yes (expected)\n");
    }

    // Now see if we can match a string that SHOULDNT match
    match = "mail-123-abc.example.com.foo.com";
    printf("Match %s <-> %s? ", regex, match);
    ret = pcre_exec(regex_compiled, regex_extra, match, strlen(match), 0, PCRE_ANCHORED, substrvec, 30);
    if ( ret < 0 ) {
        // Error occurred
        printf("no (expected) - ");
	switch (ret) {
	    case PCRE_ERROR_NOMATCH: printf("did not match\n"); break;
	    default: printf("bad regex or other error\n"); break;
	}
    } else {
        printf("YES *UNEXPECTED*\n");
    }

    // Free up the compiled regular expression.
    pcre_free(regex_compiled);

    // Free up the extra pcre
    pcre_free(regex_extra);

    return(0);
}
