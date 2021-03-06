// Copyright 2009 The Archiveopteryx Developers <info@aox.org>

#ifndef SMTP_H
#define SMTP_H

#include "saslconnection.h"
#include "list.h"


class User;
class EString;
class Address;
class SmtpCommand;


class SMTP
    : public SaslConnection
{
public:
    enum Dialect{ Smtp, Lmtp, Submit };

    SMTP( int s, Dialect=Smtp );

    void react( Event e );

    void parse();

    void execute();

    enum InputState { Command, Sasl, Chunk, Data };
    InputState inputState() const;
    void setInputState( InputState );

    Dialect dialect() const;

    void setHeloName( const EString & );
    EString heloName() const;

    class Sieve * sieve() const;

    void reset();

    User * user() const;
    void authenticated( User * );

    List<Address> * permittedAddresses();

    void addRecipient( class SmtpRcptTo * );
    List<class SmtpRcptTo> * rcptTo() const;

    void setBody( const EString & );
    EString body() const;

    bool isFirstCommand( SmtpCommand * ) const;

    void setTransactionId( const EString & );
    EString transactionId();

    void setTransactionTime( class Date * );
    class Date * transactionTime() const;

    virtual void sendChallenge( const EString & );

private:
    void parseCommand();

private:
    class SMTPData * d;

};


class LMTP
    : public SMTP
{
public:
    LMTP( int s );
};


class SMTPSubmit
    : public SMTP
{
public:
    SMTPSubmit( int s );
};


class SMTPS
    : public SMTPSubmit
{
public:
    SMTPS( int );

    void finish();

private:
    class SMTPSData * d;
};


#endif
