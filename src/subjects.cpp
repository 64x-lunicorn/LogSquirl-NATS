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
 * @file subjects.cpp
 * @brief Parsing the subjects entered, and the overlap check.
 */

#include "subjects.h"

#include <QRegularExpression>
#include <QStringList>

namespace nats_monitor {

namespace {

const QString kOne = QStringLiteral( "*" );
const QString kRest = QStringLiteral( ">" );

QStringList tokens( const QString& subject )
{
    return subject.split( QLatin1Char( '.' ) );
}

} // namespace

ParsedSubscriptions parseSubscriptions( const QString& text )
{
    static const QRegularExpression space( QStringLiteral( "\\s+" ) );
    ParsedSubscriptions parsed;
    for ( const auto& line : text.split( QLatin1Char( '\n' ) ) ) {
        const auto fields = line.split( space, Qt::SkipEmptyParts );
        if ( fields.isEmpty() ) {
            continue;
        }
        if ( fields.size() > 2 ) {
            parsed.error = QStringLiteral( "\"%1\" is more than a subject and a queue group: "
                                           "enter one subject a line." )
                               .arg( line.trimmed() );
            return parsed;
        }
        parsed.subscriptions.append( { fields.at( 0 ), fields.value( 1 ) } );
    }
    return parsed;
}

bool subjectsOverlap( const QString& a, const QString& b )
{
    const auto x = tokens( a );
    const auto y = tokens( b );
    for ( qsizetype i = 0;; ++i ) {
        const bool xEnds = i == x.size();
        const bool yEnds = i == y.size();
        if ( xEnds || yEnds ) {
            return xEnds && yEnds;
        }
        // '>' matches one token or more: the other needs one left here.
        if ( x.at( i ) == kRest || y.at( i ) == kRest ) {
            return true;
        }
        if ( x.at( i ) != kOne && y.at( i ) != kOne && x.at( i ) != y.at( i ) ) {
            return false;
        }
    }
}

bool subjectCovers( const QString& wider, const QString& narrower )
{
    const auto w = tokens( wider );
    const auto n = tokens( narrower );
    for ( qsizetype i = 0;; ++i ) {
        if ( i == w.size() || i == n.size() ) {
            return i == w.size() && i == n.size();
        }
        if ( w.at( i ) == kRest ) {
            return true; // n has a token here, and '>' takes it and any after
        }
        if ( n.at( i ) == kRest ) {
            return false; // matches more tokens than w's one here
        }
        if ( w.at( i ) != kOne && w.at( i ) != n.at( i ) ) {
            return false; // a literal, or a '*' in n against a literal
        }
    }
}

QString overlapWarning( const QList<Subscription>& subscriptions )
{
    const QString copies = QStringLiteral( "a message on both is written once for each" );
    QStringList warnings;
    for ( qsizetype i = 0; i < subscriptions.size(); ++i ) {
        for ( qsizetype j = i + 1; j < subscriptions.size(); ++j ) {
            const auto& a = subscriptions.at( i ).subject;
            const auto& b = subscriptions.at( j ).subject;
            if ( a == b ) {
                warnings << QStringLiteral( "%1 twice: a message on it is written once for each." )
                                .arg( a );
            }
            else if ( subjectCovers( a, b ) || subjectCovers( b, a ) ) {
                const auto& wider = subjectCovers( a, b ) ? a : b;
                const auto& narrower = subjectCovers( a, b ) ? b : a;
                warnings << QStringLiteral( "%1 and %2 overlap: %3; drop %2, which %1 covers." )
                                .arg( wider, narrower, copies );
            }
            else if ( subjectsOverlap( a, b ) ) {
                warnings << QStringLiteral( "%1 and %2 overlap: %3." ).arg( a, b, copies );
            }
        }
    }
    return warnings.join( QLatin1Char( ' ' ) );
}

} // namespace nats_monitor
