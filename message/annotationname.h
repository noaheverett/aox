// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef ANNOTATIONNAME_H
#define ANNOTATIONNAME_H

#include "stringlist.h"
#include "injector.h"

class EventHandler;
class Query;


class AnnotationName {
public:
    static void setup();

    static void reload( EventHandler * = 0 );
    static void rollback();
    static uint largestId();

    static void add( const String &, uint );

    static String name( uint );
    static uint id( const String & );
};


class AnnotationNameCreator
    : public HelperRowCreator
{
public:
    AnnotationNameCreator( const StringList &, class Transaction * );

private:
    Query * makeSelect();
    void processSelect( Query * );
    Query * makeCopy();

private:
    StringList names;
};


#endif
