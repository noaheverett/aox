#ifndef AUTHENTICATE_H
#define AUTHENTICATE_H

#include "command.h"
#include "string.h"


class Authenticate
    : public Command
{
public:
    Authenticate();

    void parse();
    void execute();
    void read();

private:
    class Authenticator *a;
    String t, r;
};

#endif
