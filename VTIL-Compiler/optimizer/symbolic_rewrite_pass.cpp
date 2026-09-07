// Copyright (c) 2020 Can Boluk and contributors of the VTIL Project   
// All rights reserved.   
//    
// Redistribution and use in source and binary forms, with or without   
// modification, are permitted provided that the following conditions are met: 
//    
// 1. Redistributions of source code must retain the above copyright notice,   
//    this list of conditions and the following disclaimer.   
// 2. Redistributions in binary form must reproduce the above copyright   
//    notice, this list of conditions and the following disclaimer in the   
//    documentation and/or other materials provided with the distribution.   
// 3. Neither the name of VTIL Project nor the names of its contributors
//    may be used to endorse or promote products derived from this software 
//    without specific prior written permission.   
//    
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE   
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE  
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE   
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR   
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF   
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS   
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN   
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)   
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE  
// POSSIBILITY OF SUCH DAMAGE.        
//
#include "symbolic_rewrite_pass.hpp"
#include <cstdlib>
#include <cstdio>
#include "../common/auxiliaries.hpp"
#include "mov_propagation_pass.hpp"   // WMP: vtil::optimizer::wmp_loopcarried_vrs
#include "stack_propagation_pass.hpp" // WMP: vtil::optimizer::wmp_loopcarried_stackslots

namespace vtil::optimizer
{
	// Implement the pass.
	//
	size_t isymbolic_rewrite_pass::pass( basic_block* blk, bool xblock )
	{
		// WMP_SKIP_SYMREWRITE (gated diagnostic, default off): skip symbolic
		// re-emission to bisect which apply_all pass collapses a lift-marked
		// loop-carried branch-dependent read.  Default off -> byte-identical.
		if ( std::getenv( "WMP_SKIP_SYMREWRITE" ) )
			return 0;
		// Create an instrumented symbolic virtual machine and hook execution to exit at
		// instructions that cannot be executed out-of-order.
		//
		lambda_vm<symbolic_vm> vm;
		vm.hooks.execute = [ & ] ( const instruction& ins )
		{
			// Halt if branching instruction.
			//
			if ( ins.base->is_branching() )
				return vm_exit_reason::unknown_instruction;

			// Halt if instruction is volatile.
			//
			if ( ins.is_volatile() )
				return vm_exit_reason::unknown_instruction;

			// Halt if stack pointer is reset.
			//
			if ( ins.sp_reset )
				return vm_exit_reason::unknown_instruction;

			// Halt if instruction accesses volatile registers excluding ?UD.
			//
			for ( auto& op : ins.operands )
				if ( op.is_register() && op.reg().is_volatile() && !op.reg().is_undefined() )
					return vm_exit_reason::unknown_instruction;

			// Invoke original handler.
			//
			return vm.symbolic_vm::execute( ins );
		};

		// Allocate a temporary block.
		//
		basic_block temporary_block = { blk->owner, blk->entry_vip };
		temporary_block.last_temporary_index = blk->last_temporary_index;

		for ( il_const_iterator it = blk->begin(); !it.is_end(); )
		{
			// Execute starting from the instruction.
			//
			auto [limit, rsn] = vm.run( it );

			// Create a batch translator and an instruction buffer.
			//
			std::vector<instruction> instruction_buffer;
			batch_translator translator = { &temporary_block };

			// For each register state:
			//
			for ( auto& pair : vm.register_state )
			{
				// Skip if not written, else collapse value.
				// -- TODO: Will be reworked...
				//
				bitcnt_t msb = math::msb( pair.second.bitmap ) - 1;
				if ( msb == -1 ) continue;
				bitcnt_t size = pair.second.linear_store[ msb ].size() + msb;

				register_desc k = { pair.first, size };
				auto v = vm.read_register( k ).simplify();

				// WMP_BAKEWATCH (gated diagnostic, default off): report when symbolic
				// re-emission folds a lift-marked loop-carried VR to a CONSTANT -- the
				// suspected locus of ex02's v0/v1/k0/k2 trace-seed bake (UPDATE 264).
				if ( std::getenv( "WMP_BAKEWATCH" ) && k.is_virtual() &&
				     wmp_loopcarried_vrs.count( (std::uint64_t) k.local_id ) &&
				     v->is_constant() )
					fprintf( stderr,
						"[bakewatch] symrw loopcarried VR id=0x%llx -> const 0x%llx\n",
						(unsigned long long) k.local_id,
						(unsigned long long) v->get<std::uint64_t>().value_or( 0 ) );

				// If value is unchanged, skip.
				//
				symbolic::expression v0 = symbolic::CTX( vm.reference_iterator )[ k ];
				if ( v->equals( v0 ) )
					continue;

				// If register value is not used after this instruction, skip from emitted state.
				//
				// WMP_LOOPAWARE_SYMREWRITE (opt-in, default off -> byte-identical): a
				// lift-marked loop-carried virtual register's writeback whose ONLY consumer
				// is the cross-block self-loop back-edge header read has no within-block
				// forward reader, so aux::is_used(rec=false) (path-restricted, no tracer)
				// returns false ONLY for GLOBAL regs -> a local/lane VR store is dropped ->
				// the reg-file recurrence is severed (LANE32 -> UNDEF).  Keep the store for
				// marked LCVRs so the recurrence threads via the register the body writes.
				// Mirrors WMP_LOOPAWARE_DCE.  Inert unless the env is set AND the set is
				// non-empty (only the wmpdevrit lift populates wmp_loopcarried_vrs).
				static const bool wmp_lc_sr =
					std::getenv( "WMP_LOOPAWARE_SYMREWRITE" ) != nullptr;
				const bool wmp_keep = wmp_lc_sr && k.is_virtual() &&
					wmp_loopcarried_vrs.count( (std::uint64_t) k.local_id );
				if ( wmp_keep && std::getenv( "WMP_LCVR_DBG" ) )
					fprintf( stderr, "[lcvr-symrw] keep store VR id=0x%llx\n",
						(unsigned long long) k.local_id );
				if ( !wmp_keep &&
					 !aux::is_used( { std::prev( limit ), k }, false, nullptr ) )
					continue;
				
				// Try minimizing expression size.
				//
				for ( bitcnt_t size : preferred_exp_sizes )
				{
					// Skip if above or equal.
					//
					if ( size >= v.size() ) break;

					// If all bits above [size] are matching with original value, resize.
					//
					if ( ( v >> size ).equals( v0 >> size ) )
					{
						k.bit_count = size;
						v.resize( size );
						break;
					}
				}

				// If partially inherited flags register:
				//
				if ( k.is_flags() && k.bit_count != arch::bit_count && preferred_exp_sizes.contains( 1 ) )
				{
					// For each bit:
					//
					for ( int i = 0; i < k.bit_count; i++ )
					{
						// Skip if unchanged.
						//
						auto subexp = __bt( v, i );
						if ( subexp.equals( __bt( v0, i ) ) )
							continue;
						
						// Pack registers and the expression.
						//
						auto sv = symbolic::variable::pack_all( subexp );

						// Buffer a mov instruction to the exact bit.
						//
						register_desc ks = k;
						ks.bit_offset += i;
						ks.bit_count = 1;
						instruction_buffer.emplace_back( &ins::mov, ks, translator << sv );
					}
					continue;
				}
				
				// Validate the register output.
				//
				fassert( !k.is_stack_pointer() && !k.is_read_only() );

				// Pack registers and the expression.
				//
				symbolic::variable::pack_all( v.simplify( true ) );

				// Buffer a mov instruction.
				//
				instruction_buffer.emplace_back( &ins::mov, k, translator << v );
			}

			// For each memory state:
			// -- TODO: Simplify memory state, merge if simplifies, discard if left as is.
			//
			for ( auto& [k, v] : vm.memory_state )
			{
				v.simplify();
				symbolic::expression v0 = symbolic::MEMORY( k, v.size() );

				// If value is unchanged, skip.
				//
				if ( v->equals( v0 ) )
					continue;

				// Try minimizing expression size.
				//
				for ( bitcnt_t size : preferred_exp_sizes )
				{
					// Skip if not byte-aligned.
					//
					if ( size & 7 ) continue;

					// If all bits above [size] are matching with original value, resize.
					//
					if ( ( v >> size ).equals( v0 >> size ) )
					{
						v.resize( size );
						break;
					}
				}

				// Pack registers and the expression.
				//
				symbolic::variable::pack_all( v.simplify( true ) );

				// If pointer can be rewritten as $sp + C:
				//
				operand base, offset, value;
				if ( auto displacement = ( k - symbolic::CTX[ REG_SP ] ) )
				{
					// WMP_BAKEWATCH: report a $sp memory cell folded to a CONSTANT --
					// the suspected locus of ex02's v0/v1 operand-stack carrier bake
					// (the reg-file lane is symbolic but block-INDEP => it loads a baked
					// $sp cell; UPDATE 266).
					if ( std::getenv( "WMP_BAKEWATCH" ) && v->is_constant() )
						fprintf( stderr,
							"[bakewatch] symrw $sp+0x%llx -> const 0x%llx (sz=%d)\n",
							(unsigned long long) (intptr_t) *displacement,
							(unsigned long long) v->get<std::uint64_t>().value_or( 0 ),
							(int) v.size() );
					// WMP_SYMRW_KEEPMEM (gated, default-OFF): for a lift-marked loop-carried
					// stackslot whose symbolic re-emission folded to a CONSTANT, SKIP the
					// constant store so the cell keeps the original (symbolic) MEMORY value
					// instead of the baked trace round-constant (UPDATE 267/271).  EXPERIMENT:
					// tests whether the operand-stack v0/v1 carrier stays block-connected.
					if ( std::getenv( "WMP_SYMRW_KEEPMEM" ) && v->is_constant() &&
					     wmp_loopcarried_stackslots.count( (std::int64_t) *displacement ) )
					{
						if ( std::getenv( "WMP_BAKEWATCH" ) )
							fprintf( stderr,
								"[bakewatch] symrw KEEPMEM skip $sp+0x%llx\n",
								(unsigned long long) (intptr_t) *displacement );
						continue;
					}
					// Buffer a str $sp, c, value.
					//
					instruction_buffer.emplace_back(
						&ins::str,
						REG_SP, make_imm<intptr_t>( (intptr_t) *displacement ), translator << v
					);
				}
				else
				{
					// Try to extract the offset from the compound expression.
					//
					intptr_t offset = 0;
					auto exp = symbolic::variable::pack_all( k.base ).simplify( true );
					if ( !exp->is_constant() )
					{
						using namespace symbolic::directive;

						std::vector<symbol_table_t> results;
						if ( fast_match( &results, A + U, exp ) )
						{
							exp = results.front().translate( A );
							offset = *results.front().translate( U )->get<intptr_t>();
						}
						else if ( fast_match( &results, A - U, exp ) )
						{
							exp = results.front().translate( A );
							offset = -*results.front().translate( U )->get<intptr_t>();
						}
					}

					// Translate the base address.
					//
					operand base = translator << exp;
					if ( base.is_immediate() )
					{
						operand tmp = temporary_block.tmp( base.bit_count() );
						instruction_buffer.emplace_back( &ins::mov, tmp, base );
						base = tmp;
					}

					// Buffer a str <ptr>, 0, value.
					//
					instruction_buffer.emplace_back(
						&ins::str,
						base, make_imm( offset ), translator << v
					);
				}
			}

			// Emit entire buffer.
			//
			for ( auto& ins : instruction_buffer )
				temporary_block.emplace_back( std::move( ins ) );

			// If halting instruction is not at the end of the block, add to temporary block
			// and continue from the next instruction.
			//
			if ( !limit.is_end() )
			{
				temporary_block.np_emplace_back( *limit );
				it = std::next( limit );
				temporary_block.sp_index = it.is_end() ? blk->sp_index : it->sp_index;
			}
			// Otherwise break from the loop.
			//
			else
			{
				break;
			}

			// Reset virtual machine state.
			//
			vm.reset();
		}

		// Purge simplifier cache since block iterators are now invalidated 
		// making the cache also invalid.
		//
		symbolic::purge_simplifier_state();

		// Skip rewriting if we produced larger code.
		//
		int opt_count = (int)blk->size() - (int)temporary_block.size();
		if ( opt_count <= 0 )
		{
			if ( !force ) return 0;
			opt_count = 0;
		}

		// Rewrite the stream. 
		//
		blk->assign( temporary_block );
		blk->last_temporary_index = temporary_block.last_temporary_index;
		return opt_count;
	}
};