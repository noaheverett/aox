// Copyright Oryx Mail Systems GmbH. All enquiries to info@oryx.com, please.

#ifndef SMTP_H
#define SMTP_H

#include "connection.h"


class String;
class Command;
class Mailbox;
class Address;


class SMTP : public Connection {
public:
    SMTP( int s );

    void react( Event e );

    void parse();

    virtual void helo();
    virtual void ehlo();
    virtual void lhlo();
    void rset();
    void mail();
    void rcpt();
    void store();
    void data();
    void body( String & );
    virtual void noop();
    void help();
    void quit();
    void starttls();

    Address * address();
    void respond( int, const String & );
    void sendResponses();
    bool ok() const;
    void inject();
    virtual void reportInjection();
    void rcptAnswer( class User * );
    bool writeCopy();

    enum State {
        Initial,
        MailFrom,
        RcptTo,
        Data,
        Body,
        Injecting
    };
    State state() const;

    void setHeloString();

    void sendGenericError();

private:
    class SMTPData * d;
    friend class LMTP;
};

class LMTP : public SMTP {
public:
    LMTP( int s );

    void helo();
    void ehlo();
    void lhlo();
    void reportInjection();
};

#endif
