/* Запуск: mailer popdom{какой-нибудь}; host - ящик назначения,
 * multiple users.
 * Поэтому, командная строка - -f $envfrom -b $dstbox $users;
 * для sendmail - `-b $h $u' при флагах m и f.
 */

#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sysexits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define SENDMAIL_PATH "/usr/sbin/sendmail"
#define MAIL_LOCAL_PATH "/usr/local/libexec/mail.local"

#define REPORT_HERE_CR fprintf( stderr, "At line %d of %s\n", \
          __LINE__, __FILE__ )

#define MAX_LB_LEN 200
char localbox[ MAX_LB_LEN+2 ];

int main( int argc, char* argv[] )
{
    int cpid;
    int pfds[ 2 ];
    /* struct rlimit rl; */
    /* const char* envl_to = NULL; */
    const char* envl_from = NULL;
    const char* mbox_to = NULL;
    int opt;
    int bDebugMode;
    int ista;
    int hmode;
    const char* hname = "X-Delivered-To";
    /* str_list* Recipients = NULL; */
    
    *localbox = 0;
    chdir( "/var/tmp" ); /* для корки */
    envl_from = mbox_to = NULL;
    bDebugMode = 0;
    while( ( opt = getopt( argc, argv, "b:df:m:" ) ) != EOF ) {
        switch( opt ) {
            case 'f':
                envl_from = optarg;
            break;
            case 'b':
                mbox_to = optarg;
            break;
            case 'd':
                bDebugMode = 1;
            break;
            case 'm':
                hmode = strtol( optarg, NULL, 0 );
                if( hmode == 1 )
                    hname = "X-POP3-RCPT";
                else if( hmode == 2 )
                    hname = "X-Delivered-To";
                else
                    hname = optarg;
            break;
        }
    }
    /* здесь optind показывает на envl-to получателей. Получатели - все, что
       после опций */
    if( !envl_from ) {
        fprintf( stderr, "pop3m: ERROR: no envelope-from address\n" );
        return EX_USAGE;
    }
    if( !mbox_to || !*mbox_to ) {
        fprintf( stderr, "pop3m: ERROR: no mbox-to parameter\n" );
        return EX_USAGE;
    }
    fprintf( stderr, "params: %s %s\n", envl_from, mbox_to );
    *localbox = 0;
    REPORT_HERE_CR;
    fprintf( stderr, "pt 12\n" );
    if( strlen( mbox_to ) > MAX_LB_LEN ) {
        fprintf( stderr, "pop3m: ERROR: bad destination box: %s\n",
                mbox_to );
        return EX_CONFIG;
    }
    strncpy( localbox, mbox_to, MAX_LB_LEN );
    localbox[ MAX_LB_LEN ] = 0;
    fprintf( stderr, "pt 13\n" );
    if( strlen( localbox ) <= 0 ) {
        fprintf( stderr, "pop3m: ERROR: no local box defined\n" );
        return EX_CONFIG;
    }
    
    // Now we have local box. Send it to sendmail
    fprintf( stderr, "localbox = %s\n", localbox );
    fflush( NULL );
    if( pipe( pfds ) != 0 ) {
        fprintf( stderr, "pop3m: ERROR: cannot pipe\n" );
        return EX_OSERR;
    }
    if( ( cpid = fork() ) == -1 ) {
        fprintf( stderr, "pop3m: ERROR: cannot fork\n" );
        return EX_OSERR;
    }
    if( cpid == 0 ) // child
    {
        fclose( stdin );
        close( pfds[ 1 ] );
        if( pfds[ 0 ] != 0 ) {
            if( dup2( pfds[ 0 ], 0 ) == -1 )
                exit( EX_OSERR );
            close( pfds[ 0 ] );
        }
#ifdef MAIL_LOCAL_PATH
        if( strlen( localbox ) == strspn( localbox,
              "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
              "abcdefghijklmnopqrstuvwxyz0123456789_" ) &&
            ( getuid() == 0 || geteuid() == 0 ) )
        {
            setuid(0);
            if( geteuid() == 0 ) {
                execl( MAIL_LOCAL_PATH, "mail", "-f",
                        envl_from, localbox, NULL );
                fprintf( stderr, "pop3m: fatal: cannot execute mail.local\n" );
            }
        }
#endif
        /* Fallback to sendmail */
        execl( SENDMAIL_PATH, "sendmail", "-i",
                "-f", envl_from, localbox, NULL );
        fprintf( stderr, "pop3m: fatal: cannot exec sendmail\n" );
        fflush( stderr );
        _exit( EX_OSERR );
    }
    else /* parent */
    {
        pid_t rpid;
#if !defined(CONVERT_HEADER)
        FILE* fpd = NULL;
        int i, c;
        close( pfds[ 0 ] );
        if( ( fpd = fdopen( pfds[ 1 ], "w" ) ) == NULL ) {
            kill( cpid, SIGHUP );
            fprintf( stderr, "pop3m: ERROR: no resources\n" );
            return EX_OSERR;
        }
        for( i = optind; i < argc; i++ ) {
            fprintf( fpd, "%s: %s\n", hname, argv[i] );
        }
        while( ( c = getchar() ) != EOF )
            putc( c, fpd );
        if( ferror( stdin ) || ferror( fpd ) ) {
            fprintf( stderr, "pop3m: fatal: i/o\n" );
            kill( cpid, SIGHUP );
            return EX_OSERR;
        }
        if( fflush( fpd ) == EOF ) {
            fprintf( stderr, "pop3m: fatal: i/o\n" );
            kill( cpid, SIGHUP );
            return EX_OSERR;
        }
        fclose( fpd );
#else /* CONVERT_HEADER */
        FILE* fpd = NULL;
        // Parse letter, include "To: " after first line,
        // masquerade or kill old "To:", "Cc:", "Bcc:"
        // Header lines are limited to length 2048
        //// int bAH = 0; // in addressee (To, Cc, Bcc) header
        int bNeedInsert = 1; // need insert "To:" line
        int bFirstLine = 1; // this will be the first line of message header
        int bWasLF = 1; // previous line chunk had LF
        int bNowLF;
        int bInBody = 0;
        int bSkipLine = 0;
        char buf[ 4100 ];
        //fprintf( stderr, "Parent started\n" );
        if( ( fpd = fdopen( pfds[ 1 ], "w" ) ) == NULL ) {
            kill( cpid, SIGHUP );
            fprintf( stderr, "pop3m: ERROR: no resources\n" );
            return EX_OSERR;
        }
        for( ; !ferror( stdin ) && !feof( stdin ) && !ferror( fpd );
                bWasLF = bNowLF )
        {
            if( bWasLF )
                bSkipLine = 0;
            bNowLF = 0;
            if( !fgets( buf, sizeof buf, stdin ) )
                break;
            bNowLF = ( strlen( buf ) > 0 &&
                    buf[ strlen( buf ) - 1 ] == '\n' );
            if( bInBody || !bWasLF ) {
                if( !bSkipLine )
                    fputs( buf, fpd );
                continue;
            }
            assert( bWasLF && !bInBody );
            if( *buf == '\n' ) {
                fprintf( fpd, "\n" );
                bInBody = 1;
                continue;
            }
            if( bFirstLine && strncmp( buf, "From ", 5 ) != 0 ) {
                int i;
                // Здесь continue не зовется!
                assert( bNeedInsert );
                for( i = optind; i < argc; i++ ) {
                    fprintf( fpd, "To: %s\n", hname, argv[i] );
                }
                fprintf( fpd, "X-Envelope-From: %s\n", envl_from );
                bNeedInsert = 0;
            }
            if( !strncmp( buf, "From ", 5 ) ) {
                bFirstLine = 0;
                fputs( "X-Old-From_: ", fpd );
                fputs( buf + 5, fpd );
                continue;
            }
            if( bFirstLine ) {
                time_t tt;
                time( &tt );
                bFirstLine = 0;
                fprintf( fpd, "Received: (from root@localhost)"
                        " by root@localhost;\n\t%s",
                        ctime( &tt ) );
            }
            if( bNeedInsert ) {
                int i;
                for( i = optind; i < argc; i++ ) {
                    fprintf( fpd, "To: %s\n", hname, argv[i] );
                }
                fprintf( fpd, "X-Envelope-From: %s\n", envl_from );
                bNeedInsert = 0;
            }
            if( !strncmp( buf, "To:", 3 ) ||
                    !strncmp( buf, "Cc:", 3 ) ||
                    !strncmp( buf, "Bcc:", 4 ) )
            {
                fputs( "X-Old-", fpd );
            }
            fputs( buf, fpd );
            bFirstLine = 0;
        }
        fflush( NULL );
        if( ferror( stdin ) || ferror( fpd ) ) {
            kill( cpid, SIGHUP );
            fprintf( stderr, "pop3m: ERROR: i/o\n" );
            return EX_OSERR;
        }
        fclose( fpd );
#endif /* !defined(CONVERT_HEADER) */
        // Wait for child
RetryWait:
        rpid = waitpid( cpid, &ista, 0 );
        if( rpid == -1 ) {
            fprintf( stderr, "pop3m: ERROR: waitpid(): %s\n",
                  strerror(errno) );
            return EX_OSERR;
        }
        if( WIFSTOPPED( ista ) )
            goto RetryWait;
        if( WIFSIGNALED( ista ) ) {
            fprintf( stderr, "pop3m: ERROR: child killed by signal\n" );
            return EX_OSERR;
        }
        if( WEXITSTATUS( ista ) != 0 ) {
            fprintf( stderr, "pop3m: ERROR: child returned %d\n",
                    ( int ) WEXITSTATUS( ista ) );
            return WEXITSTATUS( ista );
        }
    } // parent
    return 0;
}

#ifdef _cplusplius
extern "C"
#endif
void abort( void ) {
    _exit( 3 );
}


