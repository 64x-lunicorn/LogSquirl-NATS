/*
 * Copyright (C) 2026 LogSquirl Contributors
 *
 * This file is part of logsquirl-nats.
 *
 * logsquirl-nats is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * logsquirl-nats is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with logsquirl-nats.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @file logformat_test.cpp
 * @brief BDD tests of the Log Format definition, formats/nats_log.json.
 *
 * The file is read as LogSquirl reads a Log Format: its regexes compiled
 * with QRegularExpression and no options, its columns the named groups in
 * pattern order, its timestamp read with its timestamp formats by the rules
 * of LogSquirl's TimestampReader (src/logformat/src/timestampreader.cpp),
 * ported below for the directives a timestamp format may use.  The
 * lines come from the real formatter, so the format and the definition
 * cannot drift apart; the fields expected are written out by hand.
 */

#include <catch2/catch.hpp>

#include "lineformat.h"
#include "testconfig.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTimeZone>

#include <optional>
#include <vector>

using nats_monitor::formatLine;
using nats_monitor::formatMarker;
using nats_monitor::Message;

namespace {

/// The one format the file defines, as LogSquirl reads it.
struct LogFormat {
    QString name;
    QJsonObject definition;
    QStringList patterns;
    QString timestampField = "timestamp"; // lnav's defaults, as LogSquirl's
    QString bodyField = "body";
    QStringList timestampFormats;
};

LogFormat loadFormat()
{
    QFile file( QStringLiteral( LOGSQUIRL_NATS_TEST_SOURCE_DIR "/formats/nats_log.json" ) );
    REQUIRE( file.open( QIODevice::ReadOnly ) );
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson( file.readAll(), &error );
    INFO( error.errorString().toStdString() );
    REQUIRE( document.isObject() );

    LogFormat format;
    const auto root = document.object();
    for ( auto it = root.begin(); it != root.end(); ++it ) {
        if ( !it.key().startsWith( '$' ) ) {
            REQUIRE( format.name.isEmpty() ); // one format in the file
            format.name = it.key();
            format.definition = it.value().toObject();
        }
    }
    REQUIRE_FALSE( format.name.isEmpty() );

    const auto& def = format.definition;
    const auto regex = def.value( "regex" ).toObject();
    for ( auto it = regex.begin(); it != regex.end(); ++it ) {
        format.patterns << it.value().toObject().value( "pattern" ).toString();
    }
    if ( def.contains( "timestamp-field" ) ) {
        format.timestampField = def.value( "timestamp-field" ).toString();
    }
    if ( def.contains( "body-field" ) ) {
        format.bodyField = def.value( "body-field" ).toString();
    }
    const auto tsFormat = def.value( "timestamp-format" );
    if ( tsFormat.isArray() ) {
        for ( const auto& f : tsFormat.toArray() ) {
            format.timestampFormats << f.toString();
        }
    }
    else if ( tsFormat.isString() ) {
        format.timestampFormats << tsFormat.toString();
    }
    return format;
}

/// The format's match of @p line, the first of its patterns that matches.
QRegularExpressionMatch matchLine( const LogFormat& format, const QString& line )
{
    for ( const auto& pattern : format.patterns ) {
        const QRegularExpression re( pattern ); // no options, as LogSquirl's
        auto match = re.match( line );
        if ( match.hasMatch() ) {
            return match;
        }
    }
    return {};
}

/// The named groups of @p pattern, by the rule LogSquirl derives the
/// column order with.
QStringList namedGroups( const QString& pattern )
{
    static const QRegularExpression namedGroupRe( R"(\(\?<([a-zA-Z_]\w*)>)" );
    QStringList names;
    auto it = namedGroupRe.globalMatch( pattern );
    while ( it.hasNext() ) {
        names << it.next().captured( 1 );
    }
    return names;
}

// ── LogSquirl's TimestampReader, ported ────────────────────────────────────
//
// It mirrors LogSquirl's src/logformat/src/timestampreader.cpp (checked
// against v26.10.0 and 26.12), the same port as LogSquirl-tcpdump's test;
// it is not the real reader.  The same tokens and reading rules as the
// host's: a run of whitespace in the format matches one or more whitespace
// characters, %Y exactly four
// digits, %m %d %H %M %S one or two, %f and %L any number of digits of which
// the first three are the milliseconds, %z/%Z an optional zone ("Z", "UTC",
// "+02:00", ...) that makes the time the UTC instant it names; trailing
// whitespace is allowed, anything else left over is not.  A directive not
// ported here makes the format unreadable, so that a change of the format
// to one fails this test instead of passing unchecked.

enum class Kind { Literal, Space, Year4, Month, Day, Hour, Minute, Second, Fraction, Zone };

struct Token {
    Kind kind;
    QChar literal;
};

std::optional<std::vector<Token>> compileTimestampFormat( const QString& format )
{
    std::vector<Token> pattern;
    for ( qsizetype i = 0; i < format.size(); ++i ) {
        const auto c = format[ i ];
        if ( c.isSpace() ) {
            if ( pattern.empty() || pattern.back().kind != Kind::Space ) {
                pattern.push_back( { Kind::Space, {} } );
            }
            continue;
        }
        if ( c != '%' ) {
            pattern.push_back( { Kind::Literal, c } );
            continue;
        }
        if ( ++i >= format.size() ) {
            return std::nullopt;
        }
        switch ( format[ i ].unicode() ) {
        case 'Y':
            pattern.push_back( { Kind::Year4, {} } );
            break;
        case 'm':
            pattern.push_back( { Kind::Month, {} } );
            break;
        case 'd':
        case 'e':
            pattern.push_back( { Kind::Day, {} } );
            break;
        case 'H':
            pattern.push_back( { Kind::Hour, {} } );
            break;
        case 'M':
            pattern.push_back( { Kind::Minute, {} } );
            break;
        case 'S':
            pattern.push_back( { Kind::Second, {} } );
            break;
        case 'L':
        case 'f':
            pattern.push_back( { Kind::Fraction, {} } );
            break;
        case 'z':
        case 'Z':
            pattern.push_back( { Kind::Zone, {} } );
            break;
        case '%':
            pattern.push_back( { Kind::Literal, '%' } );
            break;
        default:
            return std::nullopt;
        }
    }
    return pattern;
}

bool isDigit( QChar c )
{
    return c >= '0' && c <= '9';
}

std::optional<int> readNumber( QStringView text, qsizetype& pos, int maxDigits )
{
    int value = 0;
    int digits = 0;
    while ( digits < maxDigits && pos < text.size() && isDigit( text[ pos ] ) ) {
        value = value * 10 + ( text[ pos ].unicode() - '0' );
        ++pos;
        ++digits;
    }
    return digits == 0 ? std::nullopt : std::optional<int>( value );
}

std::optional<int> readFraction( QStringView text, qsizetype& pos )
{
    int millis = 0;
    int digits = 0;
    for ( ; pos < text.size() && isDigit( text[ pos ] ); ++pos, ++digits ) {
        if ( digits < 3 ) {
            millis = millis * 10 + ( text[ pos ].unicode() - '0' );
        }
    }
    if ( digits == 0 ) {
        return std::nullopt;
    }
    for ( int d = digits; d < 3; ++d ) {
        millis *= 10;
    }
    return millis;
}

std::optional<int> readZone( QStringView text, qsizetype& pos )
{
    auto p = pos;
    while ( p < text.size() && text[ p ].isSpace() ) {
        ++p;
    }
    if ( p >= text.size() ) {
        return std::nullopt;
    }
    if ( text[ p ] == '+' || text[ p ] == '-' ) {
        const auto sign = text[ p ] == '-' ? -1 : 1;
        const auto digitsStart = ++p;
        QString digits;
        for ( ; p < text.size() && ( isDigit( text[ p ] ) || text[ p ] == ':' ); ++p ) {
            if ( text[ p ] != ':' ) {
                digits += text[ p ];
            }
        }
        if ( p - digitsStart < 2 ) {
            return std::nullopt;
        }
        pos = p;
        const auto hours = digits.left( 2 ).toInt();
        const auto minutes = digits.size() >= 4 ? digits.mid( 2, 2 ).toInt() : 0;
        if ( hours > 23 || minutes > 59 ) {
            return std::nullopt;
        }
        return sign * ( hours * 3600 + minutes * 60 );
    }
    const auto start = p;
    while ( p < text.size() && text[ p ].isLetter() ) {
        ++p;
    }
    pos = p;
    const auto name = text.mid( start, p - start ).toString().toUpper();
    if ( name == "Z" || name == "UTC" || name == "GMT" || name == "UT" ) {
        return 0;
    }
    return std::nullopt;
}

/// The Timestamp LogSquirl reads from the text of a timestamp field with the
/// format's timestamp formats, the first that fits winning.
std::optional<QDateTime> readTimestamp( const QStringList& formats, QStringView text )
{
    for ( const auto& format : formats ) {
        const auto pattern = compileTimestampFormat( format );
        REQUIRE( pattern ); // a directive LogSquirl knows and this port has
        int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millis = 0;
        std::optional<int> offset;
        qsizetype pos = 0;
        bool fits = true;
        for ( const auto& token : *pattern ) {
            std::optional<int> value = 0;
            switch ( token.kind ) {
            case Kind::Literal:
                fits = pos < text.size() && text[ pos ] == token.literal;
                ++pos;
                break;
            case Kind::Space:
                fits = pos < text.size() && text[ pos ].isSpace();
                while ( pos < text.size() && text[ pos ].isSpace() ) {
                    ++pos;
                }
                break;
            case Kind::Year4: {
                const auto start = pos;
                value = readNumber( text, pos, 4 );
                fits = value && pos - start == 4;
                year = value.value_or( 0 );
                break;
            }
            case Kind::Month:
                month = ( value = readNumber( text, pos, 2 ) ).value_or( 0 );
                break;
            case Kind::Day:
                day = ( value = readNumber( text, pos, 2 ) ).value_or( 0 );
                break;
            case Kind::Hour:
                hour = ( value = readNumber( text, pos, 2 ) ).value_or( 0 );
                break;
            case Kind::Minute:
                minute = ( value = readNumber( text, pos, 2 ) ).value_or( 0 );
                break;
            case Kind::Second:
                second = ( value = readNumber( text, pos, 2 ) ).value_or( 0 );
                break;
            case Kind::Fraction:
                millis = ( value = readFraction( text, pos ) ).value_or( 0 );
                break;
            case Kind::Zone:
                offset = readZone( text, pos );
                break;
            }
            if ( !fits || !value ) {
                fits = false;
                break;
            }
        }
        while ( fits && pos < text.size() && text[ pos ].isSpace() ) {
            ++pos;
        }
        if ( !fits || pos != text.size() ) {
            continue;
        }
        const QDate date( year, month, day );
        const QTime time( hour, minute, second, millis );
        if ( date.isValid() && time.isValid() ) {
            return QDateTime( date, time, QTimeZone::UTC ).addSecs( -offset.value_or( 0 ) );
        }
    }
    return std::nullopt;
}

/// 2026-11-11 10:00:00.123456 UTC, in microseconds since the epoch.
constexpr qint64 kReceived = 1794391200123456LL;

Message message( const QByteArray& subject, const QByteArray& payload )
{
    Message m;
    m.receivedUs = kReceived;
    m.subject = subject;
    m.payload = payload;
    return m;
}

/// The columns LogSquirl should show for a line.
struct Fields {
    QString timestamp, subject, reply, size, body;
};

/// Checks that the format reads @p line into @p expected.
void requireFields( const LogFormat& format, const QByteArray& line, const Fields& expected )
{
    const auto text = QString::fromUtf8( line );
    INFO( "line: " << line.left( 200 ).toStdString() );
    const auto match = matchLine( format, text );
    REQUIRE( match.hasMatch() );
    REQUIRE( match.captured( "timestamp" ) == expected.timestamp );
    REQUIRE( match.captured( "subject" ) == expected.subject );
    REQUIRE( match.captured( "reply" ) == expected.reply );
    REQUIRE( match.captured( "size" ) == expected.size );
    REQUIRE( match.captured( "body" ) == expected.body );
}

} // namespace

SCENARIO( "the Log Format defines the columns of a NATS line", "[logformat]" )
{
    const auto format = loadFormat();

    THEN( "its patterns compile as LogSquirl compiles them" )
    {
        REQUIRE_FALSE( format.patterns.isEmpty() );
        for ( const auto& pattern : format.patterns ) {
            const QRegularExpression re( pattern );
            INFO( re.errorString().toStdString() );
            REQUIRE( re.isValid() );
        }
    }

    THEN( "the columns are timestamp, subject, reply, size and body, in this order" )
    {
        for ( const auto& pattern : format.patterns ) {
            REQUIRE( namedGroups( pattern )
                     == QStringList{ "timestamp", "subject", "reply", "size", "body" } );
        }
        REQUIRE( format.timestampField == "timestamp" );
        REQUIRE( format.bodyField == "body" );
        REQUIRE_FALSE( format.timestampFormats.isEmpty() );
    }

    THEN( "the subject is an identifier, and the size the only number" )
    {
        const auto values = format.definition.value( "value" ).toObject();
        for ( const QString name : { "subject", "reply", "size" } ) {
            INFO( "field " << name.toStdString() );
            REQUIRE( values.contains( name ) );
            const auto value = values.value( name ).toObject();
            const auto kind = value.value( "kind" ).toString();
            REQUIRE( ( kind == "integer" || kind == "float" ) == ( name == "size" ) );
            REQUIRE( value.value( "identifier" ).toBool() == ( name == "subject" ) );
            REQUIRE_FALSE( value.value( "hidden" ).toBool() );
        }
    }

    THEN( "its sample lines match it" )
    {
        const auto samples = format.definition.value( "sample" ).toArray();
        REQUIRE_FALSE( samples.isEmpty() );
        for ( const auto& sample : samples ) {
            const auto line = sample.toObject().value( "line" ).toString();
            INFO( "sample: " << line.toStdString() );
            REQUIRE( matchLine( format, line ).hasMatch() );
        }
    }
}

SCENARIO( "the Log Format reads every line the formatter writes", "[logformat]" )
{
    const auto format = loadFormat();
    const QString ts = "2026-11-11 10:00:00.123456Z";

    GIVEN( "a JSON message with a reply subject" )
    {
        auto m = message( "orders.eu.created", R"({"id":1,"total":9.5})" );
        m.reply = "_INBOX.ab12";

        THEN( "every field is a column, the payload the body" )
        {
            requireFields(
                format, formatLine( m ),
                { ts, "orders.eu.created", "_INBOX.ab12", "20", R"({"id":1,"total":9.5})" } );
        }
    }

    GIVEN( "a message without a reply subject" )
    {
        THEN( "the reply column is a dash" )
        {
            requireFields( format, formatLine( message( "sensors.t1", "21.5" ) ),
                           { ts, "sensors.t1", "-", "4", "21.5" } );
        }
    }

    GIVEN( "a message with headers" )
    {
        auto m = message( "sensors.t1", "21.5" );
        m.headers = { { "Nats-Msg-Id", "42" }, { "Trace", "abc" } };

        THEN( "the headers start the body" )
        {
            requireFields( format, formatLine( m ),
                           { ts, "sensors.t1", "-", "4", "[Nats-Msg-Id=42; Trace=abc] 21.5" } );
        }
    }

    GIVEN( "headers with escaped ; ] = \\ and line breaks, and no payload" )
    {
        auto m = message( "h", "" );
        m.headers = { { "a=b", "1;2" }, { "B", "x]y" }, { "C", "l1\r\nl2\\" } };

        THEN( "the body is the header block" )
        {
            requireFields( format, formatLine( m ),
                           { ts, "h", "-", "0", R"([a\=b=1\;2; B=x\]y; C=l1\r\nl2\\])" } );
        }
    }

    GIVEN( "an empty payload" )
    {
        THEN( "the body is empty" )
        {
            requireFields( format, formatLine( message( "ping", "" ) ),
                           { ts, "ping", "-", "0", "" } );
        }
    }

    GIVEN( "text with line breaks, backslashes, a tab and control characters" )
    {
        const auto m = message( "log", QByteArray( "a\nb\r\nc\\d\te\0f\x1b", 13 ) );

        THEN( "the escaped text is the body" )
        {
            requireFields( format, formatLine( m ),
                           { ts, "log", "-", "13", "a\\nb\\r\\nc\\\\d\te\\x00f\\x1B" } );
        }
    }

    GIVEN( "text with Unicode line and paragraph separators and a NEL" )
    {
        const auto m = message( "u", "a b c\u0085d" );

        THEN( "they stay as they are, inside the body" )
        {
            requireFields( format, formatLine( m ),
                           { ts, "u", "-", "12", QString::fromUtf8( "a b c\u0085d" ) } );
        }
    }

    GIVEN( "text with leading and trailing spaces" )
    {
        THEN( "the spaces are part of the body" )
        {
            requireFields( format, formatLine( message( "s", "  x  " ) ),
                           { ts, "s", "-", "5", "  x  " } );
        }
    }

    GIVEN( "non-ASCII text" )
    {
        THEN( "it is the body as it is" )
        {
            requireFields( format, formatLine( message( "greet", "Grüße, 世界 🐿" ) ),
                           { ts, "greet", "-", "20", "Grüße, 世界 🐿" } );
        }
    }

    GIVEN( "text starting with [ or hex:" )
    {
        THEN( "the escaped first byte starts the body" )
        {
            requireFields( format, formatLine( message( "t", "[1] up" ) ),
                           { ts, "t", "-", "6", R"(\x5B1] up)" } );
            requireFields( format, formatLine( message( "t", "hex:00" ) ),
                           { ts, "t", "-", "6", R"(\x68ex:00)" } );
        }
    }

    GIVEN( "a binary payload" )
    {
        const auto m = message( "bin", QByteArray( "\x00\x01\xfe\xff", 4 ) );

        THEN( "the hex is the body" )
        {
            requireFields( format, formatLine( m ), { ts, "bin", "-", "4", "hex:0001feff" } );
        }
    }

    GIVEN( "a 4 MiB text payload" )
    {
        const auto m = message( "big", QByteArray( "0123456789abcdef" ).repeated( 262144 ) );

        // LogSquirl gives a line over MaxUiThreadLineLength (1,000,000
        // characters) no columns; this checks only that the regex covers it.
        THEN( "the regex matches it, the whole payload the body" )
        {
            requireFields( format, formatLine( m ),
                           { ts, "big", "-", "4194304", QString::fromUtf8( m.payload ) } );
        }
    }

    GIVEN( "subjects and reply subjects with $, -, _ and many tokens" )
    {
        auto m = message( "$JS.API.STREAM.INFO.my-stream_1", "{}" );
        m.reply = "$JS.ACK.my-stream_1.c.1.2.3.1794391200123456000.0";

        THEN( "they are read whole" )
        {
            requireFields( format, formatLine( m ),
                           { ts, "$JS.API.STREAM.INFO.my-stream_1",
                             "$JS.ACK.my-stream_1.c.1.2.3.1794391200123456000.0", "2", "{}" } );
        }
    }

    GIVEN( "marker lines" )
    {
        THEN( "they are read with the subject $NATS-MONITOR" )
        {
            requireFields(
                format, formatMarker( kReceived, "connected to nats://127.0.0.1:4222" ),
                { ts, "$NATS-MONITOR", "-", "34", "connected to nats://127.0.0.1:4222" } );
            requireFields( format, formatMarker( kReceived, "disconnected" ),
                           { ts, "$NATS-MONITOR", "-", "12", "disconnected" } );
        }
    }
}

SCENARIO( "the Log Format reads the receive time", "[logformat]" )
{
    const auto format = loadFormat();

    GIVEN( "lines received at different times" )
    {
        for ( const auto& [ us, expected ] :
              { std::pair{ kReceived, QDateTime( QDate( 2026, 11, 11 ), QTime( 10, 0, 0, 123 ),
                                                 QTimeZone::UTC ) },
                std::pair{ 1794391200000042LL, QDateTime( QDate( 2026, 11, 11 ),
                                                          QTime( 10, 0, 0, 0 ), QTimeZone::UTC ) },
                std::pair{ 951782399999999LL,
                           QDateTime( QDate( 2000, 2, 28 ), QTime( 23, 59, 59, 999 ),
                                      QTimeZone::UTC ) } } ) {
            auto m = message( "a", "x" );
            m.receivedUs = us;
            const auto line = QString::fromUtf8( formatLine( m ) );

            THEN( "the timestamp column is read as the UTC instant, to the millisecond: "
                  + line.toStdString() )
            {
                const auto match = matchLine( format, line );
                REQUIRE( match.hasMatch() );
                const auto timestamp = readTimestamp( format.timestampFormats,
                                                      match.captured( format.timestampField ) );
                REQUIRE( timestamp );
                REQUIRE( *timestamp == expected );
            }
        }
    }
}

SCENARIO( "the Log Format reads no line of another log", "[logformat]" )
{
    const auto format = loadFormat();

    GIVEN( "lines of other formats with a similar start" )
    {
        THEN( "they do not match" )
        {
            for ( const QString line :
                  { "2026-11-11 10:00:00,123 INFO main started",
                    "2026-11-11 10:00:00.123 orders.eu.created - 20 x",
                    "2026-11-11T10:00:00.123456Z orders.eu.created - 20 x",
                    "2026-11-11 10:00:00.123456Z orders.eu.created - twenty x" } ) {
                INFO( "line: " << line.toStdString() );
                REQUIRE_FALSE( matchLine( format, line ).hasMatch() );
            }
        }
    }
}
