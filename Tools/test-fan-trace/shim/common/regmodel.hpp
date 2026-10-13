// Fake MMIO register file shared by the hekate-side and the FanDxe-side test programs.
//
// Register accesses in both code bases look like  *(vu32 *)(base + off) = v  /  |= /  &=  /  (read).
// The shims make `vu32` a class WITHOUT data members: the "object" is only ever used through its
// address, which is the register address, so no memory is touched and no mmap is needed.
#pragma once
#include <cstdint>

uint32_t reg_read (uint64_t addr);
void     reg_write (uint64_t addr, uint32_t value);

struct vu32 {
  uint64_t addr () const { return (uint64_t)(uintptr_t)this; }
  operator uint32_t () const { return reg_read (addr ()); }
  vu32 &operator= (uint32_t v) { reg_write (addr (), v); return *this; }
  vu32 &operator= (const vu32 &o) { uint32_t v = o; reg_write (addr (), v); return *this; }
  vu32 &operator|= (uint32_t v) { reg_write (addr (), reg_read (addr ()) | v); return *this; }
  vu32 &operator&= (uint32_t v) { reg_write (addr (), reg_read (addr ()) & v); return *this; }
  vu32 &operator^= (uint32_t v) { reg_write (addr (), reg_read (addr ()) ^ v); return *this; }
};
