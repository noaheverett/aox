// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "arena.h"
#include "scope.h"
#include "event.h"
#include "string.h"
#include "configuration.h"
#include "smtpclient.h"
#include "logclient.h"
#include "file.h"
#include "loop.h"
#include "log.h"

#include <stdlib.h>
#include <stdio.h>


static int status;
static SmtpClient *client;
static const char *errstr;


int main( int argc, char *argv[] )
{
    Arena firstArena;
    Scope global( &firstArena );

    String sender;
    String recipient;
    String filename;
    bool error = false;
    int verbose = 0;

    int n = 1;
    while ( n < argc ) {
        if ( argv[n][0] == '-' ) {
            switch ( argv[n][1] ) {
            case 'f':
                if ( argc - n > 1 )
                    sender = argv[++n];
                break;

            case 'v':
                verbose++;
                break;

            default:
                error = true;
                break;
            }
        }
        else if ( recipient.isEmpty() ) {
            recipient = argv[n];
        }
        else if ( filename.isEmpty() ) {
            filename = argv[n];
        }
        else {
            error = true;
        }
        n++;
    }

    if ( error || recipient.isEmpty() ) {
        fprintf( stderr,
                 "Syntax: deliver [-v] [-f sender] recipient [filename]\n" );
        exit( -1 );
    }

    File message( filename, File::Read );
    if ( !message.valid() ) {
        fprintf( stderr, "Unable to open message file %s\n", filename.cstr() );
        exit( -1 );
    }

    String contents = message.contents();

    if ( sender.isEmpty() &&
         ( contents.startsWith( "From " ) ||
           contents.startsWith( "Return-Path:" ) ) ) {
        int i = contents.find( '\n' );
        if ( i < 0 ) {
            fprintf( stderr, "Message contains no LF\n" );
            exit( -2 );
        }
        sender = contents.mid( 0, i );
        contents = contents.mid( i+1 );
        if ( sender[0] == 'R' )
            i = sender.find( ':' );
        else
            i = sender.find( ' ' );
        sender = sender.mid( i+1 ).simplified();
            i = sender.find( ' ' );
        if ( i > 0 )
            sender.truncate( i );
        if ( sender.startsWith( "<" ) && sender.endsWith( ">" ) )
            sender = sender.mid( 1, sender.length()-2 );
    }

    if ( verbose > 0 )
        fprintf( stderr, "Sending to <%s> from <%s>\n",
                 recipient.cstr(), sender.cstr() );

    Configuration::setup( "mailstore.conf" );

    Loop::setup();

    Log l( Log::General );
    global.setLog( &l );
    LogClient::setup();

    Configuration::report();

    class DeliveryHelper : public EventHandler {
    public:
        void execute() {
            if ( client->failed() ) {
                errstr = client->error().cstr();
                status = -1;
            }
            Loop::shutdown();
        }
    };

    client = new SmtpClient( sender, contents, recipient,
                             new DeliveryHelper );
    Loop::start();

    if ( verbose > 0 && status < 0 )
        fprintf( stderr, "Error: %s\n", errstr );
    return status;
}
