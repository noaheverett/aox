// Copyright 2009 The Archiveopteryx Developers <info@aox.org>

#include "manpage.h"

#include "class.h"
#include "function.h"
#include "file.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <unistd.h>


static ManPage * mp = 0;


/*! \class ManPage manpage.h
  The ManPage class provides documentation output to a UNIX man page.

  It implements the same functions as Output, but they're not static,
  and is called when Output's static functions are called.
*/


/*! Constructs an empty man page generator which will write man pages
    in the \a dir directory. */

ManPage::ManPage( const char * dir )
    : para( false ), fd( -1 ), directory( dir )
{
    mp = this;
}


/*! Destroys the man page object, flushing and closing the generated file. */

ManPage::~ManPage()
{
    endPage();
    mp = 0;
}


/*! Returns a pointer to the most recently constructed ManPage object,
    or a null pointer if none has been constructed yet. */

ManPage * ManPage::current()
{
    return mp;
}


/*! For the moment, we do not generate introductory manual
    pages. Perhaps it would be possible. This function makes ManPage
    discard output until startHeadline() is called for a Class.
*/

void ManPage::startHeadline( Intro * )
{
    endPage();
}


/*! As Output::startHeadline(). \a c is used only to generate a
    suitable man page named.
*/

void ManPage::startHeadline( Class * c )
{
    endPage();
    EString filename = directory + "/" + c->name().lower() + ".3oryx";
    fd = ::open( filename.cstr(), O_CREAT|O_WRONLY|O_TRUNC, 0644 );
    para = true;
    output( ".\\\" generated by udoc from source code\n"
            ".TH " );
    addText( c->name() );
    output( " 3oryx x/x/x Oryx Oryx\n"
            ".nh\n"
            ".SH NAME\n" );
    addText( c->name() );
    output( " class\n"
            ".SH SYNOPSIS\n"
            "\\fC#include <" );
    addText( c->file()->name() );
    output( ">\\fR\n"
            ".SH DESCRIPTION\n" );
}


/*! As Output::startHeadline().
*/

void ManPage::startHeadline( Function * )
{
    output( ".SH " );
    para = true;
}


/*! As Output::endParagraph(). */

void ManPage::endParagraph()
{
    if ( para )
        output( "\n" );
    para = false;
}


/*! As Output::addText(). \a text is escaped (how?). */

void ManPage::addText( const EString & text )
{
    if ( !para ) {
        output( ".PP\n" );
        para = true;
    }

    EString s;
    uint i = 0;
    while ( i < text.length() ) {
        if ( text[i] == '\\' )
            s.append( "\\\\" );
        else
            s.append( text[i] );
        i++;
    }
    output( s );
}


/*! As Output::addArgument(). \a text is used italicized. */

void ManPage::addArgument( const EString & text )
{
    addText( "" );
    output( "\\fI" );
    addText( text );
    output( "\\fR" );
}


/*! As Output::addFunction(). At present this outputs \a text in the
    regular font, maybe it should use a different font?

    The class to which \a f belongs is mentioned in the "see also" section.
*/

void ManPage::addFunction( const EString & text, Function * f )
{
    addText( text );
    addClass( "", f->parent() ); // no text, but make a See Also
}


/*! As Output::addClass(). \a text is output as-is, and the name of \a
    c is remembered for later mention in the See Also section.
*/

void ManPage::addClass( const EString & text, Class * c )
{
    addText( text );
    EString n = c->name() + "(3oryx)";
    if ( !references.find( n ) )
        references.insert( new EString( n ) );
}


/*! Write \a s to the output file. */

void ManPage::output( const EString & s )
{
    if ( fd < 0 || s.isEmpty() )
        return;
    int r = ::write( fd, s.data(), s.length() );
    r = r;
}


/*! Adds a See Also section mentioning everything we've mentioned
    (using addClass()).
*/

void ManPage::addReferences()
{
    if ( references.isEmpty() )
        return;
    endParagraph();
    output( ".SH SEE ALSO\n.ad l\n" );
    SortedList<EString>::Iterator it( references );
    while ( it ) {
        EString s( *it );
        ++it;
        addText( s );
        if ( !it )
            addText( "." );
        else if ( it == references.last() )
            addText( " and " );
        else
            addText( ", " );
    }
    references.clear();
}


/*! Add boilerplate describing the author. Will need configurability. */

void ManPage::addAuthor()
{
    endParagraph();
    output( ".SH AUTHOR\n" );
    addText( "Automatically generated from source code belonging to " );
    addText( Output::owner() );
    if ( !Output::ownerHome().isEmpty() ) {
        addText( " (" );
        addText( Output::ownerHome() );
        addText( ")" );
    }
    addText( ". All rights reserved." );
    endParagraph();
}


/*! Emits the routing verbiage at the end of a manpage. */

void ManPage::endPage()
{
    if ( fd < 0 )
        return;

    addAuthor();
    addReferences();
    endParagraph();
    ::close( fd );
}
