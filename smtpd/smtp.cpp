// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "smtp.h"

#include "log.h"
#include "scope.h"
#include "string.h"
#include "address.h"
#include "parser.h"
#include "buffer.h"
#include "message.h"
#include "configuration.h"
#include "date.h"
#include "header.h"
#include "injector.h"
#include "mailbox.h"
#include "loop.h"
#include "tls.h"
#include "user.h"


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
    if ( injector && injector->done() )
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
    owner->rcptAnswer();
}


class SMTPData
{
public:
    SMTPData():
        code( 0 ), state( SMTP::Initial ),
        pipelining( false ), from( 0 ), user( 0 ), protocol( "smtp" ),
        injector( 0 ), helper( 0 ), tlsServer( 0 ), tlsHelper( 0 ),
        negotiatingTls( false )
    {}

    int code;
    StringList response;
    SMTP::State state;
    bool pipelining;
    Address * from;
    User * user;
    List<User> to;
    String body;
    String arg;
    String helo;
    String protocol;
    Injector * injector;
    SmtpDbClient * helper;
    TlsServer * tlsServer;
    SmtpTlsStarter * tlsHelper;
    String messageError;
    bool negotiatingTls;
};


/*! \class SMTP smtp.h
    The SMTP class implements a basic SMTP server.

    This is not a full MTA, merely an SMTP server that can be used for
    message injection. It will not relay to any other server.

    There is also a closely related LMTP class, a subclass of this.

    This class implements SMTP as specified by RFC 2821, with the
    extensions specified by RFC 1651 (EHLO), RFC 1652 (8BITMIME), RFC
    2197 (pipelining) and RFC 2487 (STARTTLS). In some ways, this parser
    is a little too lax.
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


/*! Destroys the server without notifying the client in any way. */

SMTP::~SMTP()
{
    Loop::removeConnection( this );
    delete d->injector;
    delete d->helper;
    d = 0;
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
        break;

    case Shutdown:
        enqueue( String( "421 Server must shut down\r\n" ) );
        Connection::setState( Closing );
        break;
    }
}


/*! Parses the SMTP/LMTP command stream and calls execution commands
    as necessary.

    Line length is limited to 2048: RFC 2821 section 4.5.3 says 512 is
    acceptable and various SMTP extensions may increase it. RFC 2822
    declares that line lengths shoudl be limited to 998 characters.

    I spontaneously declare 32768 to be big enough.
*/

void SMTP::parse()
{
    Buffer * r = readBuffer();
    while ( Connection::state() == Connected ) {
        uint i = 0;
        while ( i < r->size() && (*r)[i] != 10 )
            i++;
        if ( i >= 32768 ) {
            log( "Connection closed due to overlong line (" +
                 fn( i ) + " bytes)", Log::Error );
            respond( 500, "Line too long (maximum is 32768 bytes)" );
            Connection::setState( Closing );
            return;
        }
        if ( i >= r->size() )
            return;

        // if we can read something, TLS isn't eating our bytes
        d->negotiatingTls = false;

        // we have a line; read it
        String line = *(r->string( ++i ));
        r->remove( i );
        if ( state() == Body ) {
            body( line );
        }
        else {
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

        if ( d->state != Verifying && d->state != Body &&
             d->state != Injecting && !d->negotiatingTls )
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


/*! Changes state to account for the HELO command. Notably, pipelining
  is then not allowed.
*/

void SMTP::helo()
{
    if ( state() != Initial ) {
        respond( 503, "HELO permitted initially only" );
        return;
    }
    setHeloString();
    respond( 250, Configuration::hostname() );
    d->state = MailFrom;
}


/*! Changes state to account for the EHLO command. Since EHLO
  announces pipeline support, we support pipelines once this has been
  called.

  Note that this is called by LMTP::lhlo().
*/

void SMTP::ehlo()
{
    if ( state() != Initial ) {
        respond( 503, "HELO permitted initially only" );
        return;
    }
    setHeloString();
    respond( 250, Configuration::hostname() );
    //for the moment not
    //respond( 250, "STARTTLS" );
    //respond( 250, "PIPELINING" );
    respond( 250, "DSN" );
    d->state = MailFrom;
    d->pipelining = true;
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
    d->state = Initial;
    d->pipelining = false;
    respond( 250, "State reset" );
}


/*! mail() handles MAIL FROM. Carefully. */

void SMTP::mail()
{
    if ( state() != MailFrom ) {
        respond( 503, "Bad sequence of commands" );
        return;
    }
    d->from = address();
    if ( ok() && d->from ) {
        respond( 250, "Accepted message from " + d->from->toString() );
        d->state = RcptTo;
    }
}


/*! rcpt() handles RCPT TO. This needs to be simple, since it's more
    or less duplicated in LMTP.
*/

void SMTP::rcpt()
{
    if ( state() != RcptTo && state() != Data ) {
        respond( 503, "Must specify sender before recipient(s)" );
        return;
    }
    Address * to = address();
    if ( !to || !to->valid() ) {
        respond( 550, "Unknown address: " + to->toString() );
        return;
    }
    if ( d->user ) {
        respond( 550, "Cannot process two RCPT commands simultanously" );
        return;
    }

    d->user = new User;
    d->user->setAddress( to );
    d->user->refresh( new SmtpUserHelper( this, d->user ) );
    d->state = Verifying;
}


/*! Delivers the SMTP answer, not based on the database lookup. Should
    this use anything, e.g. from User::error()?
*/

void SMTP::rcptAnswer()
{
    Address *a = d->user->address();
    String to = a->localpart() + "@" + a->domain();

    if ( d->user && d->user->valid() ) {
        d->state = Data;
        d->to.append( d->user );
        respond( 250, "Will send to " + to );
    }
    else {
        respond( 550, to + " is not a legal destination address" );
    }
    d->user = 0;
    sendResponses();
}


/*! The DATA command is a little peculiar, and can be as simple or
    complex as one wishes when PIPELINING is implemented. We implement
    all of SMTP and LMTP DATA in one command: 503 if the command isn't
    sensible, 354 elsewhere.
*/

void SMTP::data()
{
    if ( state() != Data ) {
        respond( 503, "Must specify sender and recipient(s) first" );
        return;
    }
    if ( d->to.isEmpty() ) {
        respond( 503, "No valid recipients specified" );
    }
    else {
        respond( 354, "Go ahead (sending to " +
                 fn( d->to.count() ) + " recipients)" );
        d->state = Body;
        sendResponses();
    }
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
    if ( d->code < 100 )
        respond( 250, "OK" ); // to provide a good default

    String n = fn( d->code );
    String r;
    StringList::Iterator it( d->response.first() );
    do {
        String l = *it;
        ++it;
        r.append( n );
        if ( !it )
            r.append( " " );
        else
            r.append( "-" );
        r.append( l );
        r.append( "\r\n" );
    } while ( it );

    enqueue( r );
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
    Date now;
    now.setCurrentTime();
    String received = "Received: from " +
                      peer().address() + " (HELO " + d->helo + ") " +
                      "by " + Configuration::hostname() + " " +
                      "with " + d->protocol + "; " + now.rfc822() + "\r\n";

    d->state = Injecting;

    Message * m = new Message( received + d->body );
    m->header()->removeField( HeaderField::ReturnPath );
    m->header()->add( "Return-Path", d->from->toString() );
    if ( !m->valid() ) {
        d->messageError = m->error();
        reportInjection();
        return;
    }

    SortedList<Mailbox> * mailboxes = new SortedList<Mailbox>;
    List<User>::Iterator it( d->to.first() );
    while ( it ) {
        mailboxes->insert( it->inbox() );
        ++it;
    }

    d->helper = new SmtpDbClient( this );
    d->injector = new Injector( m, mailboxes, d->helper );
    d->helper->injector = d->injector;
    d->injector->execute();
}


/*! Reports on how message injection fared, and sets the state back to
    MailFrom.
*/

void SMTP::reportInjection()
{
    if ( d->state != Injecting )
        return;

    d->state = MailFrom;

    if ( !d->injector )
        respond( 554, d->messageError );
    else if ( d->injector->failed() )
        respond( 451, "Unable to inject" );
    else
        respond( 250, "Done" );
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

    List<User>::Iterator it( d->to.first() );
    while ( it ) {
        Address * a = it->address();
        String prefix = a->localpart() + "@" + a->domain() + ": ";
        if ( !d->injector )
            respond( 554, prefix + d->messageError );
        else if ( d->injector->failed() )
            respond( 451, prefix + "Unable to inject into mailbox " +
                     it->inbox()->name() );
        else
            respond( 250, prefix + "injected into " +
                     it->inbox()->name() );
        ++it;
        sendResponses();
    }
}
