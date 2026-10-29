#pragma once
#include <QString>
#include <iostream>

// The environment modules use this small adapter without FarSeaApp's message bus.
class AppLogStream
{
  public:
    template <class T> AppLogStream& operator<<(const T& value)
    {
        std::cout << value;
        return *this;
    }

    AppLogStream& operator<<(const QString& value)
    {
        std::cout << value.toUtf8().constData();
        return *this;
    }

    AppLogStream& operator<<(std::ostream& (*value)(std::ostream&))
    {
        value(std::cout);
        return *this;
    }
};

inline AppLogStream& Msg()
{
    static AppLogStream stream;
    return stream;
}
