// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#include "listext.h"

#include "string.h"
#include "stringlist.h"
#include "mailbox.h"
#include "user.h"


class ListextData
{
public:
    ListextData():
        reference( 0 ),
        responses( 0 ),
        extended( false ),
        returnSubscribed( false ), returnChildren( false ),
        selectSubscribed( false ), selectRemote( false ),
        selectMatchParent( false )
    {}

    Mailbox * reference;
    StringList patterns;

    uint responses;

    bool extended;
    bool returnSubscribed;
    bool returnChildren;
    bool selectSubscribed;
    bool selectRemote;
    bool selectMatchParent;
};


/*! \class Listext listext.h

    The Listext class implements the extended List command, ie. the
    List command from imap4rev1 with the extensions added since.

    The extension grammar is intentionally kept minimal, since it's
    still a draft. Currently based on draft-ietf-imapext-list-extensions-09.

    Mailstore does not support remote mailboxes, so the listext option
    to show remote mailboxes is silently ignored.

    This class contains a few utility functions used by Lsub, since
    the two share so much behaviour, namely match(), reference() and
    combinedName().
*/


/*!  Constructs an empty List handler. */

Listext::Listext()
    : d( new ListextData )
{
}


/*! Note that the extensions are always parsed, even if the no
    extension has been advertised using CAPABILITY.
*/

void Listext::parse()
{
    // list = "LIST" [SP list-select-opts] SP mailbox SP mbox-or-pat

    space();

    if ( present( "(" ) ) {
        d->extended = true;
        // list-select-opts = "(" [list-select-option
        //                    *(SP list-select-option)] ")"
        // list-select-option = "SUBSCRIBED" / "REMOTE" / "MATCHPARENT" /
        //                      option-extension
        addSelectOption( atom().lower() );
        while ( present( " " ) )
            addSelectOption( atom().lower() );
        require( ")" );
        space();
    }

    d->reference = reference();
    space();

    // mbox-or-pat = list-mailbox / patterns
    // patterns = "(" list-mailbox *(SP list-mailbox) ")"
    if ( present( "(" ) ) {
        d->extended = true;

        d->patterns.append( listMailbox() );
        while ( present( " " ) )
            d->patterns.append( listMailbox() );
        require( ")" );
    }
    else {
        d->patterns.append( listMailbox() );
    }

    // list-return-opts = "RETURN (" [return-option *(SP return-option)] ")"
    if ( present( "return (" ) ) {
        d->extended = true;

        addReturnOption( atom().lower() );
        while ( present( " " ) )
            addReturnOption( atom().lower() );
        require( ")" );
    }
    end();
}


void Listext::execute()
{
    if ( d->selectMatchParent && !d->selectRemote && !d->selectSubscribed ) {
        error( Bad, "MATCH-PARENT is not valid on its own" );
        return;
    }

    StringList::Iterator it( d->patterns.first() );
    while ( it ) {
        if ( it->isEmpty() )
            respond( "LIST \"/\" \"\"" );
        else if ( (*it)[0] == '/' )
            listChildren( Mailbox::root(), *it );
        else if ( d->reference )
            listChildren( d->reference, *it );
        else
            listChildren( imap()->user()->home(), *it );
        ++it;
    }

    finish();
}


/*! Parses and remembers the return \a option, or emits a suitable
    error. \a option must be in lower case.*/

void Listext::addReturnOption( const String & option )
{
    if ( option == "subscribed" )
        d->returnSubscribed = true;
    else if ( option == "children" )
        d->returnChildren = true;
    else
        error( Bad, "Unknown return option: " + option );
}


/*! Parses the selection \a option, or emits a suitable error. \a
    option must be lower-cased. */

void Listext::addSelectOption( const String & option )
{
    if ( option == "subscribed" )
        d->selectSubscribed = true;
    else if ( option == "remote" )
        d->selectRemote = true;
    else if ( option == "matchparent" )
        d->selectMatchParent = true;
    else
        error( Bad, "Unknown selection option: " + option );
}


/*! This extremely slow pattern matching helper checks that \a pattern
    (starting at character \a p) matches \a name (starting at
    character \a n), and returns 2 in case of match, 1 if a child of
    \a name might match, and 0 if neither is the case.
*/

uint Listext::match( const String & pattern, uint p,
                     const String & name, uint n )
{
    bool one = false;
    while ( p < pattern.length() ) {
        if ( pattern[p] == '*' || pattern[p] == '%' ) {
            bool star = false;
            while ( pattern[p] == '*' || pattern[p] == '%' ) {
                if ( pattern[p] == '*' )
                    star = true;
                p++;
            }
            uint i = n;
            if ( star )
                i = name.length();
            else
                while ( i < name.length() && name[i] != '/' )
                    i++;
            while ( i >= n && i > 0 ) {
                uint r = match( pattern, p, name, i );
                if ( r == 2 )
                    return 2;
                if ( r == 1 )
                    one = true;
                i--;
            }
        }
        else if ( p >= pattern.length() && n >= name.length() ) {
            // ran out of pattern and name at the same time. success.
            return 2;
        }
        else if ( pattern[p] == name[n] ) {
            // nothing. proceed.
        }
        else if ( pattern[p] == '/' && n > name.length() ) {
            // we ran out of name and the pattern wants a child.
            return 1;
        }
        else {
            // plain old mismatch.
            return 0;
        }
        p++;
        n++;
    }
    if ( n >= name.length() )
        return 2;
    if ( one )
        return 1;
    return 0;
}


/*! Considers whether the mailbox \a m or any of its children may match
    the pattern \a p, and if so, emits list responses. (Calls itself
    recursively to handle children.)
*/

void Listext::list( Mailbox *m, const String &p )
{
    if ( !m )
        return;

    bool matches = false;
    bool matchChildren = false;

    String name = m->name();

    uint s = 0;
    if ( p[0] != '/' && p[0] != '*' )
        s = d->reference->name().length() + 1;

    switch( match( p, 0, name, s ) ) {
    case 0:
        break;
    case 1:
        matchChildren = true;
        break;
    default:
        matches = true;
        matchChildren = true;
        break;
    }

    uint responses = d->responses;

    bool reported = false;
    if ( matches ) {
        sendListResponse( m ); // simple case: send in the "right" order
        reported = true;
    }

    if ( matchChildren )
        listChildren( m, p );

    if ( reported )
        ; // no need to repeat it
    else if ( responses < d->responses && d->selectMatchParent )
        sendListResponse( m ); // some child matched and we matchparent
    else if ( responses < d->responses && m->deleted() )
        sendListResponse( m ); // some child matched and it's deleted
}


/*! Calls list() for each child of \a mailbox using \a pattern. */

void Listext::listChildren( Mailbox * mailbox, const String & pattern )
{
    List<Mailbox> * c = mailbox->children();
    if ( c ) {
        List<Mailbox>::Iterator it( c->first() );
        while ( it ) {
            list( it, pattern );
            it++;
        }
    }
}


/*! Sends a LIST response for \a mailbox.

    Open issue: If \a mailbox is the inbox, what should we send?
    INBOX, or the fully qualified name, or the name relative to the
    user's home directory?
*/

void Listext::sendListResponse( Mailbox * mailbox )
{
    if ( !mailbox )
        return;

    StringList a;

    // set up the underlying flags
    bool exists = true;
    if ( mailbox->synthetic() || mailbox->deleted() )
        exists = false;
    bool children = false;
    if ( mailbox->children() && !mailbox->children()->isEmpty() )
        children = true;
    // matchparent also needs some flags from the caller

    // translate those flags into mailbox attributes
    if ( !exists )
        a.append( "\\noselect" );
    if ( children )
        a.append( "\\haschildren" );
    else
        a.append( "\\hasnochildren" );

    respond( "LIST (" + a.join( " " ) + ") \"/\" " + mailbox->name() );
    d->responses++;
}


/*! Parses a reference name and returns a pointer to the relevant
    mailbox. Returns a null pointer and logs an error if something is
    wrong.
*/

Mailbox * Listext::reference()
{
    String name = astring();
    Mailbox * m;
    if ( name[0] == '/' )
        m = Mailbox::obtain( name, false );
    else if ( name.isEmpty() )
        m = imap()->user()->home();
    else
        m = Mailbox::obtain( imap()->user()->home()->name() + "/" + name,
                             false );
    if ( !m )
        error( No, "Cannot find reference name " + name );
    return m;
}


/*! Returns the combined name formed by interpreting the mailbox \a
    name in the context of the \a reference mailbox.

    If \a name starts with a slash, \a reference isn't dereferenceds,
    so it can be a null pointer. \a name need not be a valid mailbox
    name, it can also be e.g. a pattern.
*/

String Listext::combinedName( Mailbox * reference, const String & name )
{
    if ( name.startsWith( "/" ) )
        return name;
    else if ( reference )
        return reference->name() + "/" + name;

    return imap()->user()->home()->name() + "/" + name;
}


/*! Parses and returns a list-mailbox. This is the same as an atom(),
    except that the three additional characters %, * and ] are
    accepted.
*/

String Listext::listMailbox()
{
    String result;
    char c = nextChar();
    if ( c == '"' || c == '{' )
        return string();
    while ( c > ' ' && c < 127 &&
            c != '(' && c != ')' && c != '{' &&
            c != '"' && c != '\\' )
    {
        result.append( c );
        step();
        c = nextChar();
    }
    if ( result.isEmpty() )
        error( Bad, "list-mailbox expected, saw: " + following() );
    return result;
}
