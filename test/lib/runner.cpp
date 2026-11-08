/* -*- C++ -*- copyright (c) 2014-2026 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "brick-shelltest.h"

#include <cerrno>
#include <cstring>
#include <sys/stat.h>

/*
 * The suite owns the loop devices and the device-mapper namespace of the
 * host, so two runs collide even when they use different test directories or
 * build trees: the second one finds the devices of the first and skips its
 * tests.  Serialize every run on the host with a lock in the LVM run
 * directory and let a second run wait for the first.  LVM_TEST_PARALLEL asks
 * the harness to tolerate that interference, so no lock is taken then.
 */
static const char test_lock_path[] = DEFAULT_RUN_DIR "/lvm2-testsuite.lock";

static void lock_test_run( Flock &lock )
{
    if ( hasenv( "LVM_TEST_PARALLEL" ) )
        return;

    /* Shared run directory; lvm creates it the same way. */
    if ( mkdir( DEFAULT_RUN_DIR, 0777 ) && errno != EEXIST ) {
        std::cerr << "Cannot create " << DEFAULT_RUN_DIR << ": "
                  << strerror( errno ) << ", running without the lock."
                  << std::endl;
        return;
    }

    switch ( lock.try_acquire() ) {
    case 1:
        return;
    case 0:
        break;
    default:
        std::cerr << "Cannot lock " << test_lock_path << ": "
                  << strerror( errno ) << ", running without it." << std::endl;
        return;
    }

    std::cout << "Waiting for another test run of the suite to finish..."
              << std::flush;

    if ( lock.acquire() < 0 ) {
        std::cerr << " cannot lock " << test_lock_path << ": "
                  << strerror( errno ) << std::endl;
        return;
    }

    std::cout << " done." << std::endl;
}

int main(int argc, const char **argv)
{
    /* Held until main returns, that is for the whole run. */
    Flock lock( test_lock_path );

    lock_test_run( lock );

    try {
        return brick::shelltest::run( argc, argv, "LVM_TEST_FLAVOUR" );
    } catch (std::exception const& e) {
        std::cout << "Exception: " << e.what() << "\n";
    }

    return 1;
}

