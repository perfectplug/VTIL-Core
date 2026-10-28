// WMP fork test: the loop-aware dead-code-elimination guard must be
// ALLOWLIST-SCOPED and default-OFF.
//
// The guard in optimizer/dead_code_elimination_pass.cpp refuses to nop an
// otherwise-dead write to a lift-marked loop-carried virtual register, but ONLY
// when WMP_LOOPAWARE_DCE is set AND the written VR's local_id is in
// vtil::optimizer::wmp_loopcarried_vrs. (wmpdevrit needs this because a
// loop-carried recurrence's store can look dead after other passes fold the
// loop-header read; removing it severs the recurrence.)
//
// Scenario: one block with two dead virtual-register writes (t_allow, t_other),
// neither read. DCE normally removes both. We allowlist ONLY t_allow, then:
//   * WMP_LOOPAWARE_DCE set   -> t_allow is KEPT, t_other is removed (scoped).
//   * WMP_LOOPAWARE_DCE unset -> BOTH removed (byte-identical to upstream).
// t_other being removed in every case proves the guard does not over-reach.
//
// The VTIL-Tests binary is run both with and without WMP_LOOPAWARE_DCE in CI, so
// this one assertion body (which branches on getenv) covers the gated-on path and
// the default-off byte-identical path. The flag is read once into a static const
// in the pass, so its value is fixed for the whole process — consistent with the
// env the suite is launched under.
#include "doctest.h"
#include <vtil/vtil>
#include <vtil/arch>
#include <vtil/optimizer-tests>
#include <cstdint>
#include <cstdlib>
#include <set>

// The cross-project allowlists (populated by the wmpdevrit lift before
// apply_all). Declared here exactly as in the optimizer headers.
namespace vtil::optimizer
{
    extern std::set<std::uint64_t> wmp_loopcarried_vrs;
    extern std::set<std::int64_t>  wmp_loopcarried_stackslots;
}

DOCTEST_TEST_CASE( "WMP loop-aware DCE guard is allowlist-scoped and default-off" )
{
    vtil::logger::log( "\n\n>> %s \n", __FUNCTION__ );
    const bool wmp_on = std::getenv( "WMP_LOOPAWARE_DCE" ) != nullptr;
    vtil::logger::log( "   WMP_LOOPAWARE_DCE = %s\n", wmp_on ? "SET" : "unset" );

    constexpr std::uint64_t kAllow = 0x5706;   // allowlisted loop-carried VR
    constexpr std::uint64_t kOther = 0x5707;   // not allowlisted

    auto block = vtil::basic_block::begin( 0x1337, vtil::architecture_amd64 );
    vtil::register_desc t_allow( vtil::register_local, kAllow, vtil::arch::bit_count, 0 );
    vtil::register_desc t_other( vtil::register_local, kOther, vtil::arch::bit_count, 0 );

    block->mov( t_allow, (uintptr_t) 0x1234 );   // dead: never read
    block->mov( t_other, (uintptr_t) 0x5678 );   // dead: never read
    block->vexit( 0ull );

    vtil::optimizer::wmp_loopcarried_vrs.clear();
    vtil::optimizer::wmp_loopcarried_vrs.insert( kAllow );   // allowlist t_allow only

    vtil::debug::dump( block->owner );
    vtil::optimizer::dead_code_elimination_pass{}( block->owner );
    vtil::debug::dump( block->owner );

    vtil::optimizer::wmp_loopcarried_vrs.clear();

    bool allow_kept = false, other_kept = false;
    for ( auto& ins : *block )
    {
        if ( ins.base == &vtil::ins::mov && !ins.operands.empty() && ins.operands[ 0 ].is_register() )
        {
            auto id = ins.operands[ 0 ].reg().local_id;
            if ( id == kAllow ) allow_kept = true;
            if ( id == kOther ) other_kept = true;
        }
    }

    // The un-allowlisted dead write is removed in every configuration: the guard
    // never over-reaches.
    CHECK_FALSE( other_kept );

    if ( wmp_on )
        CHECK( allow_kept );        // guard fires: recurrence store preserved
    else
        CHECK_FALSE( allow_kept );  // default off: byte-identical (store removed)
}
