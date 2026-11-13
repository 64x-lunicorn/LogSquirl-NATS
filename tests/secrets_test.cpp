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
 * @file secrets_test.cpp
 * @brief BDD tests for wiping a secret: what the one wiped and a copy that
 *        shares its data hold afterwards.
 *
 * That the memory is overwritten cannot be read back once it is let go:
 * that is OPENSSL_cleanse()'s.  That a session keeps no secret is
 * auth_test.cpp's.
 */

#include <catch2/catch.hpp>

#include "secrets.h"

using nats_monitor::wipe;

SCENARIO( "a wiped secret is gone from it, and only from it", "[secrets]" )
{
    GIVEN( "a password as UTF-8 bytes" )
    {
        QByteArray bytes( "s3cret" );

        WHEN( "it is wiped" )
        {
            wipe( bytes );

            THEN( "it is empty" )
            {
                REQUIRE( bytes.isEmpty() );
            }
        }

        WHEN( "a copy that shares its data is wiped" )
        {
            auto copy = bytes;
            wipe( copy );

            THEN( "the copy is empty, and the original still holds the password" )
            {
                REQUIRE( copy.isEmpty() );
                REQUIRE( bytes == "s3cret" );
            }
        }
    }

    GIVEN( "a password as a string" )
    {
        QString text( "s3cret" );

        WHEN( "it is wiped" )
        {
            wipe( text );

            THEN( "it is empty" )
            {
                REQUIRE( text.isEmpty() );
            }
        }

        WHEN( "a copy that shares its data is wiped" )
        {
            auto copy = text;
            wipe( copy );

            THEN( "the copy is empty, and the original still holds the password" )
            {
                REQUIRE( copy.isEmpty() );
                REQUIRE( text == "s3cret" );
            }
        }
    }
}
