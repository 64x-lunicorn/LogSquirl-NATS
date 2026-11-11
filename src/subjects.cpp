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

const QString kAnyToken = QStringLiteral( "*" ); ///< Matches one token.
const QString kAnyTail = QStringLiteral( ">" );  ///< As the last token: one or more.

QStringList tokens( const QString& subject )
{
    return subject.split( QLatin1Char( '.' ) );
}

/// Why NATS would refuse @p subject; empty if it takes it.
QString subjectProblem( const QString& subject )
{
    const auto parts = tokens( subject );
    for ( qsizetype i = 0; i < parts.size(); ++i ) {
        const auto& token = parts.at( i );
        if ( token.isEmpty() ) {
            return QStringLiteral( "a token is empty (\"..\", or a '.' at either end)" );
        }
        if ( token != kAnyToken && token != kAnyTail
             && ( token.contains( kAnyToken ) || token.contains( kAnyTail ) ) ) {
            return QStringLiteral( "* and > must each be a token of their own" );
        }
        if ( token == kAnyTail && i != parts.size() - 1 ) {
            return QStringLiteral( "> must be the last token" );
        }
    }
    return {};
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
        if ( const auto problem = subjectProblem( fields.at( 0 ) ); !problem.isEmpty() ) {
            parsed.error
                = QStringLiteral( "%1 is not a valid subject: %2." ).arg( fields.at( 0 ), problem );
            return parsed;
        }
        parsed.subscriptions.append( { fields.at( 0 ), fields.value( 1 ) } );
    }
    return parsed;
}

bool subjectsOverlap( const QString& a, const QString& b )
{
    const auto aTokens = tokens( a );
    const auto bTokens = tokens( b );
    for ( qsizetype i = 0;; ++i ) {
        const bool aEnds = i == aTokens.size();
        const bool bEnds = i == bTokens.size();
        if ( aEnds || bEnds ) {
            return aEnds && bEnds;
        }
        const auto& x = aTokens.at( i );
        const auto& y = bTokens.at( i );
        // '>' matches one token or more: the other has one left here.
        if ( x == kAnyTail || y == kAnyTail ) {
            return true;
        }
        if ( x != kAnyToken && y != kAnyToken && x != y ) {
            return false;
        }
    }
}

bool subjectCovers( const QString& wider, const QString& narrower )
{
    const auto widerTokens = tokens( wider );
    const auto narrowerTokens = tokens( narrower );
    for ( qsizetype i = 0;; ++i ) {
        if ( i == widerTokens.size() || i == narrowerTokens.size() ) {
            return i == widerTokens.size() && i == narrowerTokens.size();
        }
        const auto& w = widerTokens.at( i );
        const auto& n = narrowerTokens.at( i );
        if ( w == kAnyTail ) {
            return true; // narrower has a token here, and '>' takes it and any after
        }
        if ( n == kAnyTail ) {
            return false; // matches more tokens than wider's one here
        }
        if ( w != kAnyToken && w != n ) {
            return false; // a literal, or a '*' in narrower against a literal
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
            const bool aCoversB = subjectCovers( a, b );
            if ( a == b ) {
                warnings << QStringLiteral( "%1 twice: a message on it is written once for each." )
                                .arg( a );
            }
            else if ( aCoversB || subjectCovers( b, a ) ) {
                const auto& wider = aCoversB ? a : b;
                const auto& narrower = aCoversB ? b : a;
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
