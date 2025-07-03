#include "wrapping_integers.hh"
#include "debug.hh"

using namespace std;

Wrap32 Wrap32::wrap( uint64_t n, Wrap32 zero_point )
{
  return Wrap32( ( n + zero_point.raw_value_ ) % ( 1ULL << 32 ) );
}

uint64_t Wrap32::unwrap( Wrap32 zero_point, uint64_t checkpoint ) const
{
  uint32_t offset = raw_value_ - zero_point.raw_value_;
  uint64_t base = checkpoint > offset ? ( ( checkpoint - offset ) + ( 1ULL << 31 ) ) : 0;
  return ( base & ~0xFFFFFFFFULL ) + offset;
}
