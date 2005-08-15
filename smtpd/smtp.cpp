// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "smtp.h"

#include "configuration.h"
#include "stringlist.h"
#include "injector.h"
#include "message.h"
#include "address.h"
#include "mailbox.h"
#include "parser.h"
#include "buffer.h"
#include "string.h"
#include "header.h"
#include "scope.h"
#include "loop.h"
#include "date.h"
#include "file.h"
#include "user.h"
#include "tls.h"
#include "log.h"

// time()
#include <time.h>
// getpid()
#include <sys/types.h>
#include <unistd.h>


uint sequence;


class SmtpDbClient: public EventHandler
{
public:
    SmtpDbClient( SMTP * s );
    void execute();

    SMTP * owner;
    Injector * injector;
};


SmtpDbClient::SmtpDbClient( SMTP * s )
    : EventHandler(), owner( s ), injector( 0 )
{
}


void SmtpDbClient::execute()
{
    if ( !injector || !injector->done() )
        return;

    if ( injector->failed() &&
         injector->message()->header() ) {
        Header * h = injector->message()->header();
        String id = h->messageId();
        if ( !id.isEmpty() )
            log( "Message-ID: " + id );
        String from;
        HeaderField * f = h->field( HeaderField::From );
        if ( f )
            from = f->value();
        if ( !from.isEmpty() )
            log( "From: " + from );
    }

    owner->reportInjection();
}


class SmtpTlsStarter: public EventHandler
{
public:
    SmtpTlsStarter( SMTP * s );
    void execute();

    SMTP * owner;
};


SmtpTlsStarter::SmtpTlsStarter( SMTP * s )
    : EventHandler(), owner( s )
{
}


void SmtpTlsStarter::execute()
{
    owner->starttls();
}


class SmtpUserHelper: public EventHandler
{
public:
    SmtpUserHelper( SMTP * s, User * u );
    void execute();

    SMTP * owner;
    User * user;
};


SmtpUserHelper::SmtpUserHelper( SMTP * s, User * u )
    : EventHandler(), owner( s ), user( u )
{
}


void SmtpUserHelper::execute()
{
    owner->rcptAnswer( user );
}


class SMTPData
    : public Garbage
{
public:
    SMTPData():
        code( 0 ), state( SMTP::Initial ),
        from( 0 ), protocol( "smtp" ),
        injector( 0 ), helper( 0 ), tlsServer( 0 ), tlsHelper( 0 ),
        negotiatingTls( false )
    {}

    int code;
    StringList response;
    SMTP::State state;
    Address * from;
    List<User> to;
    String body;
    String arg;
    String helo;
    String protocol;
    Injector * injector;
    SmtpDbClient * helper;
    TlsServer * tlsServer;
    SmtpTlsStarter * tlsHelper;
    bool negotiatingTls;
};


/*! \class SMTP smtp.h
    The SMTP class implements a basic SMTP server.

    This is not a full MTA, merely an SMTP server that can be used for
    message injection. It will not relay to any other server.

    There is also a closely related LMTP class, a subclass of this.

    This class implements SMTP as specified by RFC 2821, with the
    extensions specified by RFC 1651 (EHLO), RFC 1652 (8BITMIME), and
    RFC 2487 (STARTTLS). In some ways, this parser is a little too
    lax.
*/

/*!  Constructs an (E)SMTP server for socket \a s. */

SMTP::SMTP( int s )
    : Connection( s, Connection::SmtpServer ), d( new SMTPData )
{
    respond( 220, "ESMTP + LMTP " + Configuration::hostname() );
    sendResponses();
    setTimeoutAfter( 1800 );
    Loop::addConnection( this );
}


void SMTP::react( Event e )
{
    switch ( e ) {
    case Read:
        setTimeoutAfter( 1800 );
        parse();
        break;

    case Timeout:
        log( "Idle timeout" );
        enqueue( String( "421 Timeout\r\n" ) );
        Connection::setState( Closing );
        break;

    case Connect:
    case Error:
    case Close:
        close();
        break;

    case Shutdown:
        enqueue( String( "421 Server must shut down\r\n" ) );
        break;
    }
}


/*! Parses the SMTP/LMTP command stream and calls execution commands
    as necessary.

    Line length is limited to 2048: RFC 2821 section 4.5.3 says 512 is
    acceptable and various SMTP extensions may increase it. RFC 2822
    declares that line lengths should be limited to 998 characters.

    I spontaneously declare 262144 to be big enough.
*/

void SMTP::parse()
{
    Buffer * r = readBuffer();
    while ( Connection::state() == Connected ) {
        uint i = 0;
        while ( i < r->size() && (*r)[i] != 10 )
            i++;
        if ( i >= 262144 ) {
            log( "Connection closed due to overlong line (" +
                 fn( i ) + " bytes)", Log::Error );
            respond( 500, "Line too long (legal maximum is 998 bytes)" );
            Connection::setState( Closing );
            return;
        }
        if ( i >= r->size() )
            return;

        // if we can read something, TLS isn't eating our bytes
        d->negotiatingTls = false;

        // we have a line; read it
        String line = r->string( ++i );
        r->remove( i );
        if ( d->state == Body ) {
            body( line );
        }
        else {
            log( "Received: '" + line.stripCRLF() + "'", Log::Debug );
            i = 0;
            while ( i < line.length() &&
                    line[i] != ' ' && line[i] != 13 && line[i] != 10 )
                i++;
            String cmd = line.mid( 0, i ).lower();
            if ( cmd == "mail" || cmd == "rcpt" ) {
                while ( i < line.length() && line[i] != ':' )
                    i++;
                cmd = line.mid( 0, i++ ).lower().simplified();
            }
            d->arg = line.mid( i );
            if ( cmd == "helo" )
                helo();
            else if ( cmd == "ehlo" )
                ehlo();
            else if ( cmd == "lhlo" )
                lhlo();
            else if ( cmd == "rset" )
                rset();
            else if ( cmd == "mail from" )
                mail();
            else if ( cmd == "rcpt to" )
                rcpt();
            else if ( cmd == "data" )
                data();
            else if ( cmd == "noop" )
                noop();
            else if ( cmd == "help" )
                help();
            else if ( cmd == "starttls" )
                starttls();
            else if ( cmd == "quit" )
                quit();
            else
                respond( 500, "Unknown command (" + cmd.upper() + ")" );
        }

        if ( d->code )
            sendResponses();
    }
}


/*! Parses the HELO string, massages it for logging purposes and does
    nothing more. We may not like the string, but we can't do anything
    about it.
*/

void SMTP::setHeloString()
{
    Parser822 p( d->arg );
    p.whitespace();
    d->helo = p.domain();
}


/*! Changes state to account for the HELO command.
*/

void SMTP::helo()
{
    if ( d->state != Initial && d->state != MailFrom ) {
        respond( 503, "HELO permitted initially only" );
        return;
    }
    setHeloString();
    respond( 250, Configuration::hostname() );
    d->state = MailFrom;
}


/*! Changes state to account for the EHLO command.

    Note that this is called by LMTP::lhlo().
*/

void SMTP::ehlo()
{
    if ( d->state != Initial && d->state != MailFrom ) {
        respond( 503, "HELO permitted initially only" );
        return;
    }
    setHeloString();
    respond( 250, Configuration::hostname() );
    //for the moment not
    //respond( 250, "STARTTLS" );
    respond( 250, "DSN" );
    d->state = MailFrom;
    d->protocol = "esmtp";
}


/*! LHLO is an LMTP-only command; it's not supported in SMTP. */

void SMTP::lhlo()
{
    respond( 500, "You seem to be speaking LMTP, not SMTP" );
}


/*! Sets the server back to its initial state. */

void SMTP::rset()
{
    d->state = MailFrom;
    respond( 250, "State reset" );
}


/*! mail() handles MAIL FROM. Carefully. */

void SMTP::mail()
{
    if ( d->state != MailFrom ) {
        respond( 503, "Bad sequence of commands" );
        return;
    }
    if ( d->arg.mid( 0,2 ) == "<>" ) {
        log( "Received message from <>" );
        respond( 250, "Accepted message from mailer-daemon" );
        d->state = RcptTo;
        return;
    }
    d->from = address();
    if ( ok() && d->from ) {
        log( "Received message from " + d->from->toString() );
        respond( 250, "Accepted message from " + d->from->toString() );
        d->state = RcptTo;
    }

    d->to.clear();
    sendResponses();
}


/*! rcpt() handles RCPT TO.
*/

void SMTP::rcpt()
{
    if ( d->state != RcptTo || d->state == Data ) {
        respond( 503, "Must specify sender before recipient(s)" );
        return;
    }
    Address * to = address();
    if ( !to ) {
        respond( 550, "Unknown address" );
        return;
    }
    if ( !to->valid() ) {
        respond( 550, "Unknown address " + to->toString() );
        return;
    }

    User * u = new User;
    u->setAddress( to );
    u->refresh( new SmtpUserHelper( this, u ) );
}


/*! Delivers the SMTP answer for \a u, based on the database lookup.

    Should this report e.g. User::error()?
*/

void SMTP::rcptAnswer( User * u )
{
    Address *a = u->address();
    String to = a->localpart() + "@" + a->domain();

    if ( u && u->valid() ) {
        d->to.append( u );
        respond( 250, "Will send to " + to );
        log( "Delivering message to " + to );
        d->state = Data;
    }
    else {
        respond( 550, to + " is not a legal destination address" );
    }
    sendResponses();
}


/*! The DATA command is a little peculiar, having the BODY phase. We
    implement all of SMTP and LMTP DATA in one command: 503 if the
    command isn't sensible, 354 elsewhere.
*/

void SMTP::data()
{
    if ( d->state != Data ) {
        respond( 503, "Bad sequence of commands" );
        return;
    }

    // if a client sends lots of bad addresses, this results in 'go
    // ahead (sending to 0 recipients'.
    respond( 354,
             "Go ahead (" + fn( d->to.count() ) + " recipients)" );
    d->state = Body;
}


/*! Appends the single \a line to the body of the message sent. Undoes
    dot-stuffing and does the final injection once the dot is seen.
*/

void SMTP::body( String & line )
{
    int i = line.length() ;
    if ( i > 0 && line[i-1] == 10 )
        i--;
    if ( i > 0 && line[i-1] == 13 )
        i--;
    line.truncate( i );
    if ( i == 1 && line[0] == '.' ) {
        inject();
    }
    else if ( line[0] == '.' ) {
        d->body.append( line.mid( 1 ) );
        d->body.append( "\r\n" );
    }
    else {
        d->body.append( line );
        d->body.append( "\r\n" );
    }
}


/*! In order to implement NOOP, one properly should check that there
    are no arguments. But in order to simplify this, we don't. We really
    do nothing.
*/

void SMTP::noop()
{
    respond( 250, "Fine." );
}


/*! Our HELP implementation is as simple as can be. This too does not
    check that no arguments have been passed.
*/

void SMTP::help()
{
    respond( 250, "See http://www.oryx.com" );
}


/*! Starts an orderly connection close. */

void SMTP::quit()
{
    log( "Closing connection due to QUIT command", Log::Debug );
    respond( 221, "Have a nice day." );
    Connection::setState( Closing );
}


/*! Turns TLS on on the connection.

    Note the evil case sensitivity: This function is called
    starttls(), similar to the other smtp-verb functions in SMTP,
    while the Connection function that does the heavy lifting is
    called startTls().
*/

void SMTP::starttls()
{
    if ( hasTls() ) {
        respond( 502, "Already using TLS" );
        return;
    }

    d->negotiatingTls = true;

    if ( !d->tlsServer ) {
        d->tlsHelper = new SmtpTlsStarter( this );
        d->tlsServer = new TlsServer( d->tlsHelper, peer(), "SMTP" ); // ? LMTP?
    }

    if ( !d->tlsServer->done() )
        return;

    respond( 220, "Start negotiating TLS now." );
    sendResponses();
    log( "Negotiating TLS", Log::Debug );
    startTls( d->tlsServer );
}


/*! This helper function parses the (first part of the) command line
    and extracts a supplied address. If no address is present, it
    returns a null pointer and sends a good error code and message.

*/

Address * SMTP::address()
{
    Parser822 p( d->arg );

    p.whitespace(); // to be flexible - it's not strictly legal
    if ( p.next() != '<' ) {
        respond( 503, "Must have '<' before address " + d->arg );
        return 0;
    }
    p.step();
    String localpart = p.dotAtom();
    if ( localpart.isEmpty() ) {
        respond( 503, "Empty localparts are not allowed" );
        return 0;
    }
    if ( p.next() != '@' ) {
        respond( 503, "Need @ between localpart and domain" );
        return 0;
    }
    p.step();
    String domain = p.dotAtom();
    if ( domain.isEmpty() ) {
        respond( 503, "Empty domains are not allowed" );
        return 0;
    }
    if ( p.next() != '>' ) {
        respond( 503, "Need > after address " + d->arg );
        return 0;
    }
    p.step();
    p.whitespace();

    AddressParser a( localpart + "@" + domain );
    if ( !a.error().isEmpty() ) {
        respond( 503, "Parse error: " + a.error() );
        return 0;
    }
    if ( a.addresses()->count() != 1 ) {
        respond( 503, "Internal error: That parsed as " +
                 fn( a.addresses()->count() ) +
                 " addresses, not 1" );
        return 0;
    }

    return new Address( *a.addresses()->first() );
}


/*! Sets the SMTP response code to \a c and adds \a s to the list of
  response lines.

  If \a c is zero the response code is not changed. If \a s is empty,
  no response line is added.

  \a s must be a single line and not contains CR or LF.
*/

void SMTP::respond( int c, const String & s )
{
    if ( c )
        d->code = c;
    if ( !s.isEmpty() )
        d->response.append( new String( s ) );
}


/*! Sends the response(s) that have been built up by calls to
    respond(), and clears the response buffer for the next command.
*/

void SMTP::sendResponses()
{
    if ( !d->code )
        respond( 250, "OK" ); // to provide a good default

    String n = fn( d->code );
    StringList::Iterator it( d->response );
    do {
        String r;
        String l = *it;
        ++it;
        r.append( n );
        if ( !it )
            r.append( " " );
        else
            r.append( "-" );
        r.append( l );
        log( "Sending response '" + r + "'",
             d->code >= 400 ? Log::Error : Log::Debug );
        r.append( "\r\n" );
        enqueue( r );
    } while ( it );

    write();

    d->code = 0;
    d->response.clear();
}


/*! This convenience function returns true if no error has been
  observed so far, and false else.

  An error is defined as any 4xx or 5xx response code.
*/

bool SMTP::ok() const
{
    if ( d->code < 400 )
        return true;
    else
        return false;
}


/*! Returns the SMTP/LMTP state of this server. The state starts as
    Initial and proceeeds through the commands.*/

SMTP::State SMTP::state() const
{
    return d->state;
}


/*! Injects the message into the mailstore.

    This function does all message-level syntax checking, starts
    injection, and either calls reportInjection() or arranges for a
    callback to that function.
*/

void SMTP::inject()
{
    d->state = Injecting;
    Scope x( new Log( Log::SMTP ) );

    Date now;
    now.setCurrentTime();
    String received( "Received: from " );
    received.append( peer().address() );
    received.append( " (HELO " );
    received.append( d->helo );
    received.append( ") by " );
    received.append( Configuration::hostname() );
    received.append( " with " );
    received.append( d->protocol );
    received.append( "; " );
    received.append( now.rfc822() );
    received.append( "\r\n" );

    Message * m = new Message( received + d->body );
    m->header()->removeField( HeaderField::ReturnPath );
    if ( d->from )
        m->header()->add( "Return-Path", d->from->toString() );

    SortedList<Mailbox> * mailboxes = new SortedList<Mailbox>;
    List<User>::Iterator it( d->to );
    while ( it ) {
        mailboxes->insert( it->inbox() );
        ++it;
    }

    d->helper = new SmtpDbClient( this );
    m->setInternalDate( now.unixTime() );
    d->injector = new Injector( m, mailboxes, d->helper );
    d->helper->injector = d->injector;
    d->injector->execute();
}


/*! Writes a copy of the message into the message-copy-directory, if
    appropriate. Returns true if the copy was successfully written or
    there was no need to write it, false if there was an error.
*/

bool SMTP::writeCopy()
{
    String mc( Configuration::text( Configuration::MessageCopy ) );
    if ( mc == "none" )
        return true;
    bool failed = true;
    if ( d->injector && !d->injector->failed() )
        failed = false;
    if ( mc == "delivered" && failed )
        return true;
    if ( mc == "errors" && !failed )
        return true;

    String copy( Configuration::text( Configuration::MessageCopyDir ) );
    copy.append( '/' );
    copy.append( fn( time(0) ) );
    copy.append( '-' );
    copy.append( fn( getpid() ) );
    copy.append( '-' );
    copy.append( fn( ++sequence ) );

    String e;
    if ( d->injector && d->injector->failed() ) {
        e = "Error: Injector: " + d->injector->error();
        copy.append( "-err" );
    }

    File f( copy, File::ExclusiveWrite );
    if ( !f.valid() ) {
        log( "Could not open " + copy + " for writing", Log::Disaster );
        return false;
    }

    f.write( "From: " );
    if ( d->from )
        f.write( d->from->toString() );
    else
        f.write( "<>" );
    f.write( "\n" );

    List<User>::Iterator it( d->to );
    while ( it ) {
        f.write( "To: " );
        f.write( it->address()->toString() );
        f.write( "\n" );
        ++it;
    }

    if ( !e.isEmpty() ) {
        f.write( e );
        f.write( "\n" );
    }
    f.write( "\n" );

    f.write( d->body );

    // XXX: How about some error checking here?

    return true;
}


/*! Reports on how message injection fared, and sets the state back to
    MailFrom.
*/

void SMTP::reportInjection()
{
    if ( d->state != Injecting )
        return;

    d->state = MailFrom;

    if ( d->injector->failed() ) {
        respond( 451, d->injector->error() );
    }
    else {
        d->injector->announce();
        respond( 250, "Done" );
    }

    sendResponses();
    commit();
    d->from = 0;
    d->to.clear();
    d->body = "";
}


/*! \class LMTP smtp.h
    The LMTP class a slightly modified SMTP to provide LMTP.

    Most of the logic is in SMTP; LMTP merely modifies the logic a
    little by reimplementating a few functions.

    LMTP is defined in RFC 2033. Note that it has no specified port number.
*/

/*!  Constructs a plain LMTP server answering file descriptor \a s. */

LMTP::LMTP( int s )
    : SMTP( s )
{
    // no separate work
}


/*! This reimplementation disable HELO. */

void LMTP::helo()
{
    respond( 500, "This is LMTP, not SMTP. Please use LHLO." );
}


/*! This reimplementation disables EHLO. */

void LMTP::ehlo()
{
    helo();
}


/*! lhlo() handles the LMTP variety of HELO, LHLO. LHLO is essentially
    equivalent to the ESMTP command EHLO, so that's how we implement it.
*/

void LMTP::lhlo()
{
    SMTP::ehlo();
    d->protocol = "lmtp";
}


void LMTP::reportInjection()
{
    if ( d->state != Injecting )
        return;

    d->state = MailFrom;

    if ( Configuration::text( Configuration::MessageCopy ) == "all" ||
         ( Configuration::text( Configuration::MessageCopy ) == "errors" &&
           ( !d->injector || d->injector->failed() ) ) )
        writeCopy();

    List<User>::Iterator it( d->to );
    while ( it ) {
        Address * a = it->address();
        String prefix = a->localpart() + "@" + a->domain() + ": ";
        if ( d->injector->failed() )
            respond( 451, prefix + d->injector->error() );
        else
            respond( 250, prefix + "injected into " +
                     it->inbox()->name() );
        ++it;
    }

    if ( d->injector && !d->injector->failed() )
        d->injector->announce();

    sendResponses();

    d->from = 0;
    d->to.clear();
    d->body = "";
}
