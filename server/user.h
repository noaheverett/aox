// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef USER_H
#define USER_H

#include "event.h"

#include "string.h"
#include "list.h"

class Mailbox;
class Address;
class Query;


class User: public EventHandler
{
public:
    User();

    void setLogin( const String & );
    String login() const;

    void setSecret( const String & );
    String secret() const;

    void setInbox( Mailbox * & );
    Mailbox * inbox() const;

    void setAddress( Address * );
    Address * address();

    uint id() const;

    void refresh( EventHandler * user );
    Query *create( EventHandler * user );
    void rename( const String & newLogin, EventHandler * user );
    void changeSecret( const String & newSecret, EventHandler * user );
    void remove( EventHandler * user );

    bool valid();
    bool exists();

    String error() const;

    static List<User> * users();

    Mailbox * home() const;

    void execute();

    enum State { Refreshed, Nonexistent, Unverified };
    State state() const;

private:
    void createHelper();
    void renameHelper();
    void refreshHelper();
    void removeHelper();

private:
    class UserData * d;
};


#endif
