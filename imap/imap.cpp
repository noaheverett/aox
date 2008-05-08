// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "imap.h"

#include "log.h"
#include "list.h"
#include "scope.h"
#include "string.h"
#include "buffer.h"
#include "mailbox.h"
#include "eventloop.h"
#include "imapsession.h"
#include "configuration.h"
#include "imapparser.h"
#include "command.h"
#include "user.h"
#include "tls.h"
#include "handlers/capability.h"


static bool endsWithLiteral( const String *, uint *, bool * );


class IMAPData
    : public Garbage
{
public:
    IMAPData()
        : state( IMAP::NotAuthenticated ), reader( 0 ),
          prefersAbsoluteMailboxes( false ),
          runningCommands( false ), runCommandsAgain( false ),
          readingLiteral( false ),
          literalSize( 0 ), session( 0 ), mailbox( 0 ),
          bytesArrived( 0 )
    {
        uint i = 0;
        while ( i < IMAP::NumClientCapabilities )
            clientCapabilities[i++] = false;
    }

    IMAP::State state;

    Command * reader;

    String str;

    bool prefersAbsoluteMailboxes;
    bool runningCommands;
    bool runCommandsAgain;
    bool readingLiteral;
    uint literalSize;

    List< Command > commands;

    ImapSession *session;
    Mailbox *mailbox;

    uint bytesArrived;

    bool clientCapabilities[IMAP::NumClientCapabilities];
};


/*! \class IMAP imap.h
    This class implements the IMAP server as seen by clients.

    This class is responsible for interacting with IMAP clients, and for
    overseeing the operation of individual command handlers. It looks at
    client input to decide which Command to defer the real work to, and
    ensures that the handler is called at the appropriate times.

    Each IMAP object has a state() (RFC 3501 section 3), and may possess
    other state information, such as the user() logged in or a
    session(). The Idle state (RFC 2177) is also kept here.

    The IMAP class parses incoming commands as soon as possible and
    may keep several commands executing at a time, if the client
    issues that. It depends on Command::group() to decide whether each
    parsed Command can be executed concurrently with the already
    running Command objects.
*/

/*! This setup function expects to be called from ::main().

    It reads and validates any relevant configuration variables, and
    logs a disaster if it encounters an error.
*/

void IMAP::setup()
{
}


/*! Creates an IMAP server on file descriptor \a s, and sends an
    initial OK[CAPABILITY...] response to the client.
*/

IMAP::IMAP( int s )
    : SaslConnection( s, Connection::ImapServer ), d( new IMAPData )
{
    if ( s < 0 )
        return;

    enqueue( "* OK [CAPABILITY " +
             Capability::capabilities( this ) + "] " +
             Configuration::hostname() + " Archiveopteryx IMAP Server\r\n" );
    setTimeoutAfter( 120 );
    EventLoop::global()->addConnection( this );
}


/*! Handles the incoming event \a e as appropriate for its type. */

void IMAP::react( Event e )
{
    d->bytesArrived += readBuffer()->size();
    switch ( e ) {
    case Read:
        parse();
        if ( d->bytesArrived > 32768 && state() == NotAuthenticated ) {
            log( ">32k received before login" );
            enqueue( "* BYE overlong login sequence\r\n" );
            Connection::setState( Closing );
            if ( d->reader ) {
                Scope s( d->reader->log() );
                d->reader->read();
            }
        }
        break;

    case Timeout:
        if ( state() != Logout ) {
            log( "Idle timeout" );
            enqueue( "* BYE Tempus fugit\r\n" );
        }
        Connection::setState( Closing );
        if ( d->reader ) {
            Scope s( d->reader->log() );
            d->reader->read();
        }
        if ( d->session )
            d->session->end();
        break;

    case Connect:
        break;

    case Error:
    case Close:
        if ( state() != Logout )
            log( "Unexpected close by client" );
        if ( d->session )
            d->session->end();
        break;

    case Shutdown:
        enqueue( "* BYE server shutdown\r\n" );
        if ( d->session )
            d->session->end();
        break;
    }

    runCommands();
    expireCommands();

    d->bytesArrived -= readBuffer()->size();

    if ( timeout() == 0 ||
         ( e == Read && state() != NotAuthenticated ) ) {
        switch ( state() ) {
        case NotAuthenticated:
            setTimeoutAfter( 120 );
            break;
        case Authenticated:
        case Selected:
            if ( idle() )
                setTimeoutAfter( 10800 ); // three-hour timeout while IDLE
            else
                setTimeoutAfter( 1800 ); // inactive client
            break;
        case Logout:
            break;
        }

    }
}


/*! Reads input from the client, and feeds it to the appropriate Command
    handlers.
*/

void IMAP::parse()
{
    Scope s;
    Buffer * r = readBuffer();

    while ( true ) {
        // We read a line of client input, possibly including literals,
        // and create a Command to deal with it.
        if ( !d->readingLiteral && !d->reader ) {
            bool plus = false;
            String * s;
            uint n;

            // Do we have a complete line yet?
            s = r->removeLine();
            if ( !s )
                return;

            d->str.append( *s );

            if ( endsWithLiteral( s, &n, &plus ) ) {
                d->str.append( "\r\n" );
                d->readingLiteral = true;
                d->literalSize = n;

                if ( !plus )
                    enqueue( "+ reading literal\r\n" );
            }

            // Have we finished reading the entire command?
            if ( !d->readingLiteral ) {
                addCommand();
                d->str.truncate();
            }
        }
        else if ( d->readingLiteral ) {
            // Have we finished reading a complete literal?
            if ( r->size() < d->literalSize )
                return;

            d->str.append( r->string( d->literalSize ) );
            r->remove( d->literalSize );
            d->readingLiteral = false;
        }
        else if ( d->reader ) {
            // If a Command has reserve()d input, we just feed it.
            Scope s( d->reader->log() );
            d->reader->read();
            if ( d->reader )
                return;
        }
    }
}


/*! This function parses enough of the command line to create a Command,
    and then uses it to parse the rest of the input.
*/

void IMAP::addCommand()
{
    // Be kind to the old man Arnt, who cannot unlearn his SMTP habits
    if ( d->str == "quit" )
        d->str = "arnt logout";

    ImapParser * p = new ImapParser( d->str );

    String tag = p->tag();
    if ( !p->ok() ) {
        enqueue( "* BAD " + p->error() + "\r\n" );
        log( p->error(), Log::Info );
        return;
    }

    p->require( " " );

    String name = p->command();
    if ( !p->ok() ) {
        enqueue( "* BAD " + p->error() + "\r\n" );
        log( p->error(), Log::Error );
        return;
    }

    Command * cmd = Command::create( this, tag, name, p );

    if ( !cmd ) {
        if ( Command::create( this, tag, tag, p ) )
            enqueue( "* OK  Hint: An IMAP command is prefixed by a tag. "
                     "The command is the\r\n"
                     "* OK  second word on the line, after the tag. In "
                     "your command, " + name.quoted() + "\r\n"
                     "* OK  is the command and " + tag.quoted() +
                     " is the tag.\r\n" );
        enqueue( tag + " BAD No such command: " + name + "\r\n" );
        log( "Unknown command. Line: " + p->firstLine().quoted(),
             Log::Error );
        return;
    }

    Scope x( cmd->log() );
    ::log( "First line: " + p->firstLine(), Log::Debug );
    d->commands.append( cmd );
}


/*! Returns the current state of this IMAP session, which is one of
    NotAuthenticated, Authenticated, Selected and Logout.
*/

IMAP::State IMAP::state() const
{
    return d->state;
}


/*! Sets this IMAP connection to be in state \a s. The initial value
    is NotAuthenticated.
*/

void IMAP::setState( State s )
{
    if ( s == d->state )
        return;
    d->state = s;
    String name;
    switch ( s ) {
    case NotAuthenticated:
        name = "not authenticated";
        break;
    case Authenticated:
        name = "authenticated";
        break;
    case Selected:
        name = "selected";
        break;
    case Logout:
        name = "logout";
        break;
    };
    log( "Changed to " + name + " state", Log::Debug );
}


/*! Returns true if the server has no particular work to do to server
    the peer(), and false if it's currently working on behalf of peer().

    If there are no commands, a connection is idle(). If the command
    currently being executed is Idle, the connection is also idle.
*/

bool IMAP::idle() const
{
    List<Command>::Iterator i( d->commands );
    while ( i ) {
        Command * c = i;
        ++i;
        switch ( c->state() ) {
        case Command::Unparsed:
            return false;
            break;
        case Command::Blocked:
            return false;
            break;
        case Command::Executing:
            if ( c->name() != "idle" )
                return false;
            break;
        case Command::Finished:
            return false;
            break;
        case Command::Retired:
            break;
        }
    }
    
    return true;
}


/*! Notifies the IMAP object that \a user was successfully
    authenticated. This changes the state() of the IMAP object to
    Authenticated.
*/

void IMAP::setUser( User * user )
{
    log( "Authenticated as user " + user->login().ascii() );
    SaslConnection::setUser( user );
    setState( Authenticated );
}


/*! Reserves input from the connection for \a command.

    When more input is available, Command::read() is
    called. Command::finish() releases control.
*/

void IMAP::reserve( Command * command )
{
    d->reader = command;
}


/*! Causes any blocked commands to be executed if possible.
*/

void IMAP::unblockCommands()
{
    if ( d->runningCommands )
        d->runCommandsAgain = true;
    else
        runCommands();
}


/*! Calls Command::execute() on all currently operating commands, and
    if possible calls Command::emitResponses() and retires those which
    can be retired.
*/

void IMAP::runCommands()
{
    d->runningCommands = true;
    d->runCommandsAgain = true;

    while ( d->runCommandsAgain ) {
        d->runCommandsAgain = false;
        log( "IMAP::runCommands, " + fn( d->commands.count() ) + " commands",
             Log::Debug );

        // run all currently executing commands once
        List< Command >::Iterator i( d->commands );
        while ( i ) {
            Command * c = i;
            ++i;
            Scope s( c->log() );
            if ( c->state() == Command::Executing ) {
                if ( c->ok() )
                    c->execute();
                else
                    c->finish();
            }
        }

        // emit responses for zero or more finished commands and
        // retire them. we also emit all error responses.
        i = d->commands.first();
        bool deferredResponse = false;
        while ( i ) {
            if ( i->state() == Command::Finished && d->reader == i )
                d->reader = 0;
            if ( i->state() == Command::Finished &&
                 ( !deferredResponse || !i->ok() ) ) {
                i->emitResponses();
                if ( i->state() == Command::Finished )
                    deferredResponse = true;
            }
            ++i;
        }

        // we may be able to start new commands. if any commands are
        // running, then following commands in the same group can be
        // started.
        i = d->commands.first();
        while ( i &&
                i->state() != Command::Executing &&
                i->state() != Command::Finished )
            i++;

        // if not, then the oldest unparsed or blocked message
        // determines which group can be executed.
        if ( !i ) {
            i = d->commands.first();
            while ( i &&
                    i->state() != Command::Unparsed &&
                    i->state() != Command::Blocked )
                i++;
        }

        // if we have a leading command, we can parse and execute
        // followers in the same group.
        if ( i ) {
            log( "IMAP::runCommands found leading command with tag " +
                 i->tag() + ", group " + fn( i->group() ) + " and state " +
                 fn( i->state() ),
                 Log::Debug );
            Command * g = i;
            while ( i &&
                    ( i->state() == Command::Executing ||
                      i->state() == Command::Finished ||
                      i->state() == Command::Retired ) )
                i++;
            while ( g && i &&
                    ( i->state() == Command::Unparsed ||
                      i->state() == Command::Blocked ) &&
                    ( g == i ||
                      ( g->group() > 0 && g->group() == i->group() ) ) ) {
                Command * c = i;
                ++i;
                Scope s( c->log() );
                if ( !c->validIn( d->state ) ) {
                    c->error( Command::Bad, "Not permitted in this state" );
                }
                else if ( c->ok() ) {
                    if ( c->state() == Command::Unparsed )
                        c->parse();
                    if ( c->group() != g->group() ) {
                        // c's group changed, we have to stop
                        if ( c->group() == Command::Unparsed )
                            c->setState( Command::Blocked );
                        g = 0;
                    }
                    else if ( c->ok() ) {
                        c->setState( Command::Executing );
                        c->execute();
                    }
                    else {
                        // it failed, but we go on.
                    }
                }
            }
        }
        else {
            uint unparsed = 0;
            uint blocked = 0;
            uint executing = 0;
            i = d->commands.first();
            while ( i ) {
                switch ( i->state() ) {
                case Command::Unparsed:
                    unparsed++;
                    break;
                case Command::Blocked:
                    blocked++;
                    break;
                case Command::Executing:
                    executing++;
                    break;
                case Command::Finished:
                    break;
                case Command::Retired:
                    break;
                }
                ++i;
            }
            if ( unparsed || blocked || executing )
                log( "IMAP::runCommands found no leading commmand, but " +
                     fn( unparsed ) + " unparsed, " +
                     fn( blocked ) + " blocked and " +
                     fn( executing ) + " executing commands.",
                     Log::Debug );
        }
    }

    d->runningCommands = false;
}


/*! Removes all commands that have finished executing from d->commands.
*/

void IMAP::expireCommands()
{
    List< Command >::Iterator i( d->commands );
    while ( i ) {
        if ( i->state() == Command::Retired )
            d->commands.take( i );
        else
            ++i;
    }
}


/*! Executes \a c once, provided it's in the right state, and emits its
    responses.
*/

void IMAP::run( Command * c )
{
    if ( c->state() != Command::Executing )
        return;

    Scope s( c->log() );

    if ( c->ok() )
        c->execute();
    else
        c->finish();
}


/*  This static helper function returns true if \a s ends with an IMAP
    literal specification. If so, it sets \a *n to the number of bytes
    in the literal, and \a *plus to true if the number had a trailing
    '+' (for LITERAL+). Returns false if it couldn't find a literal.
*/

static bool endsWithLiteral( const String *s, uint *n, bool *plus )
{
    if ( !s->endsWith( "}" ) )
        return false;

    uint i = s->length() - 2;
    if ( (*s)[i] == '+' ) {
        *plus = true;
        i--;
    }

    uint j = i;
    while ( i > 0 && (*s)[i] >= '0' && (*s)[i] <= '9' )
        i--;

    if ( (*s)[i] != '{' )
        return false;

    bool ok;
    *n = s->mid( i+1, j-i ).number( &ok );

    return ok;
}


/*! Switches to Selected state and operates on the mailbox session \a
    s. If the object already had a session, ends the previous session.
*/

void IMAP::beginSession( ImapSession * s )
{
    if ( d->session == s )
        return;
    if ( d->session )
        d->session->end();
    d->session = s;
    setState( Selected );
    log( "Starting session on mailbox " + s->mailbox()->name().ascii() );
}


/*! Returns a pointer to the ImapSession object associated with this
    IMAP server, or 0 if there is none (which can happen only if the
    server is not in the Selected state).
*/

ImapSession *IMAP::session() const
{
    return d->session;
}


/*! This function deletes any existing ImapSession associated with this
    server, whose state changes to Authenticated. It does nothing
    unless the server has a session().
*/

void IMAP::endSession()
{
    Session * s = d->session;
    if ( !s )
        return;
    setState( Authenticated );
    d->session = 0;
    s->end();
}


class IMAPSData
    : public Garbage
{
public:
    IMAPSData() : tlsServer( 0 ), helper( 0 ) {}
    TlsServer * tlsServer;
    String banner;
    class ImapsHelper * helper;
};

class ImapsHelper: public EventHandler
{
public:
    ImapsHelper( IMAPS * connection ) : c( connection ) {}
    void execute() { c->finish(); }

private:
    IMAPS * c;
};

/*! \class IMAPS imap.h

    The IMAPS class implements the old wrapper trick still commonly
    used on port 993. As befits a hack, it is a bit of a hack, and
    depends on the ability to empty its writeBuffer().
*/

/*! Constructs an IMAPS server on file descriptor \a s, and starts to
    negotiate TLS immediately.
*/

IMAPS::IMAPS( int s )
    : IMAP( s ), d( new IMAPSData )
{
    String * tmp = writeBuffer()->removeLine();
    if ( tmp )
        d->banner = *tmp;
    d->helper = new ImapsHelper( this );
    d->tlsServer = new TlsServer( d->helper, peer(), "IMAPS" );
    EventLoop::global()->removeConnection( this );
}


/*! Handles completion of TLS negotiation and sends the banner. */

void IMAPS::finish()
{
    if ( !d->tlsServer->done() )
        return;
    if ( !d->tlsServer->ok() ) {
        log( "Cannot negotiate TLS", Log::Error );
        close();
        return;
    }

    startTls( d->tlsServer );
    enqueue( d->banner + "\r\n" );
}


/*! Returns true if the client has shown that it supports a given \a
    capability, and false if this is still unknown.
*/

bool IMAP::clientSupports( ClientCapability capability ) const
{
    return d->clientCapabilities[capability];
}


/*! Records that the client supports \a capability. The initial value
    is valse for all capabilities, and there is no way to disable a
    capability once enabled.
*/

void IMAP::setClientSupports( ClientCapability capability )
{
    d->clientCapabilities[capability] = true;
}


/*! Returns a list of all Command objects currently known by this IMAP
    server. First received command first. Commands in all states may
    be in the list, although Retired should be unusual.

*/

List<Command> * IMAP::commands() const
{
    return &d->commands;
}


void IMAP::sendChallenge( const String &s )
{
    enqueue( "+ "+ s +"\r\n" );
}


/*! Records that the IMAP client likes to see its mailbox names in
    absolute form (ie. /users/kiki/lists/mja instead of lists/mja)
    if \a b is true, and that it prefers relative names otherwise.
    The initial value is false.
*/

void IMAP::setPrefersAbsoluteMailboxes( bool b )
{
    d->prefersAbsoluteMailboxes = b;
}


/*! Returns whatever setPrefersAbsoluteMailboxes() set. */

bool IMAP::prefersAbsoluteMailboxes() const
{
    return d->prefersAbsoluteMailboxes;
}
