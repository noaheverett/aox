#include "authenticate.h"

#include "arena.h"
#include "scope.h"
#include "buffer.h"
#include "imap.h"
#include "sasl/mechanism.h"


/*! \class Authenticate authenticate.h
    Initiates SASL authentication (RFC 3501, �6.2.2)

    This class oversees the SASL challenge-response negotiation, using a
    SaslMechanism subclass to handle the details of the client-selected
    authentication mechanism.

    (The details of SASL-IR probably are handled here still, aren't they?)
*/

/*! \reimp */

Authenticate::Authenticate()
        : m( 0 ), r( 0 )
{
}


/*! Parses the initial arguments to AUTHENTICATE (at least a mechanism
    name, and perhaps a SASL initial response as well).
*/

void Authenticate::parse()
{
    space();
    t = atom().lower();

    // Accept a Base64-encoded SASL initial response.
    if ( nextChar() == ' ' ) {
        char c;
        space();
        r = new String;
        while ( ( ( c = nextChar() ) >= '0' && c <= '9' ) ||
                ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) ||
                c == '+' || c == '/' || c == '=' )
        {
            step();
            r->append( c );
        }
    }

    end();
}


/*! Creates a SaslMechanism corresponding to the selected mechanism, and
    uses it to participate in a challenge-response negotiation until we
    reach a decision.

    Typically, we create a handler and issue a challenge, and are called
    again to read the response, which we accept or reject after a quick
    chat with the database.
*/

void Authenticate::execute()
{
    // First, create a mechanism handler.

    if ( !m ) {
        m = SaslMechanism::create( t, this );
        if ( !m ) {
            error( No, "Mechanism " + t + " not supported" );
            return;
        }
        imap()->reserve( this );

        // Does it accept a SASL initial response? Do we have one?
        if ( m->state() == SaslMechanism::AwaitingInitialResponse ) {
            if ( r )
                m->readResponse( r->de64() );
            else
                m->setState( SaslMechanism::IssuingChallenge );
            r = 0;
        }
    }

    // Now, feed the handler until it can make up its mind.

    while ( !m->done() ) {
        if ( m->state() == SaslMechanism::IssuingChallenge ) {
            String c = m->challenge().e64();

            if ( !m->done() ) {
                imap()->enqueue( "+ "+ c +"\r\n" );
                m->setState( SaslMechanism::AwaitingResponse );
                r = 0;
                return;
            }
        }
        else if ( m->state() == SaslMechanism::AwaitingResponse && r ) {
            if ( *r == "*" ) {
                error( Bad, "authentication terminated" );
                finish();
                return;
            }
            m->readResponse( r->de64() );
            r = 0;
        }

        if ( !m->done() ) {
            m->query();
            if ( m->state() == SaslMechanism::Authenticating )
                return;
        }
    }

    if ( m->state() == SaslMechanism::Succeeded ) {
        imap()->setUid( m->uid() );
        imap()->setLogin( m->login() );
    }
    else {
        error( No, "Sorry" );
    }

    imap()->reserve( 0 );
    finish();
}


/*! Tries to read a single response line from the client. Upon return,
    r points to the response, or is 0 if no response could be read.
*/

void Authenticate::read()
{
    r = imap()->readBuffer()->removeLine();
}
