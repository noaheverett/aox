// Copyright 2009 The Archiveopteryx Developers <info@aox.org>

#include "cram-md5.h"

#include "estring.h"
#include "entropy.h"
#include "configuration.h"
#include "user.h"
#include "md5.h"

#include <time.h>


/*! \class CramMD5 cram-md5.h
    Implements CRAM-MD5 authentication (RFC 2195)

    We issue a challenge, and expect the client to respond with username
    and the HMAC-MD5 digest of the challenge keyed with a shared secret.
    We accept the request only if the digest matches our re-computation
    based on the stored secret from the database.
*/


/*! Creates a cram-md5 SASL authentication object on behalf of \a c */

CramMD5::CramMD5( EventHandler *c )
    : SaslMechanism( c, SaslMechanism::CramMD5 )
{
}


EString CramMD5::challenge()
{
    EString hn( Configuration::hostname() );
    EString random( Entropy::asString( 12 ).e64() );

    if ( hn.isEmpty() || hn.find( '.' ) < 0 )
        hn = "oryx.invalid";

    challengeSent = "<" + random + "@" + hn + ">";

    return challengeSent;
}


void CramMD5::parseResponse( const EString &s )
{
    uint i = s.length();
    while ( i > 0 && s[i] != ' ' )
        i--;

    if ( i == 0 ) {
        log( "Syntax error in client response (no space)" );
        setState( Failed );
        return;
    }

    setLogin( s.mid( 0, i ) );
    setSecret( s.mid( i+1 ).lower() );
    setState( Authenticating );
    execute();
}


void CramMD5::verify()
{
    if ( Configuration::toggle( Configuration::AuthAnonymous ) &&
         user() && user()->login() == "anonymous" )
        setState( Succeeded );
    else if ( secret().utf8() ==
              MD5::HMAC( storedSecret().utf8(), challengeSent ).hex() )
        setState( Succeeded );
    else
        setState( Failed );
}


void CramMD5::setChallenge( const EString &s )
{
    challengeSent = s;
}
