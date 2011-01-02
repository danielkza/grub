/* dl.c - arch-dependent part of loadable module support */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2002,2004,2005,2007,2009  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/dl.h>
#include <grub/elf.h>
#include <grub/misc.h>
#include <grub/err.h>
#include <grub/mm.h>

/* Check if EHDR is a valid ELF header.  */
grub_err_t
grub_arch_dl_check_header (void *ehdr)
{
  Elf_Ehdr *e = ehdr;

  /* Check the magic numbers.  */
  if (e->e_ident[EI_CLASS] != ELFCLASS64
      || e->e_ident[EI_DATA] != ELFDATA2LSB
      || e->e_machine != EM_IA_64)
    return grub_error (GRUB_ERR_BAD_OS, "invalid arch specific ELF magic");

  return GRUB_ERR_NONE;
}

#define MASK20 ((1 << 20) - 1)
#define MASK19 ((1 << 19) - 1)

static void
add_value_to_slot13_20 (Elf_Word *addr, grub_uint32_t value, int slot)
{
  grub_uint32_t *p __attribute__ ((aligned (1)));
  switch (slot)
    {
    case 0:
      p = (grub_uint32_t *) (addr + 2);
      *p = (((((*p >> 2) & MASK20) + value) & MASK20) << 2) | (*p & ~(MASK20 << 2));
      break;
    case 1:
      p = (grub_uint32_t *) ((grub_uint8_t *) addr + 7);
      *p = (((((*p >> 3) & MASK20) + value) & MASK20) << 3) | (*p & ~(MASK20 << 3));
      break;
    case 2:
      p = (grub_uint32_t *) ((grub_uint8_t *) addr + 12);
      *p = (((((*p >> 4) & MASK20) + value) & MASK20) << 4) | (*p & ~(MASK20 << 4));
      break;
    }
}

static grub_uint8_t nopm[5] =
  {
    /* [MLX]       nop.m 0x0 */
    0x05, 0x00, 0x00, 0x00, 0x01
  };

static grub_uint8_t jump[0x20] =
  {
    /* ld8 r16=[r15],8 */
    0x02, 0x80, 0x20, 0x1e, 0x18, 0x14,
    /* mov r14=r1;; */
    0xe0, 0x00, 0x04, 0x00, 0x42, 0x00,
    /* nop.i 0x0 */
    0x00, 0x00, 0x04, 0x00,
    /* ld8 r1=[r15] */
    0x11, 0x08, 0x00, 0x1e, 0x18, 0x10,
    /* mov b6=r16 */
    0x60, 0x80, 0x04, 0x80, 0x03, 0x00,
    /* br.few b6;; */
    0x60, 0x00, 0x80, 0x00
  };

struct ia64_trampoline
{
  /* nop.m */
  grub_uint8_t nop[5];
  /* movl r15 = addr*/
  grub_uint8_t addr_hi[6];
  grub_uint8_t e0;
  grub_uint8_t addr_lo[4];
  grub_uint8_t jump[0x20];
};

static void
make_trampoline (struct ia64_trampoline *tr, grub_uint64_t addr)
{
  grub_memcpy (tr->nop, nopm, sizeof (tr->nop));
  tr->addr_hi[0] = ((addr & 0xc00000) >> 18);
  tr->addr_hi[1] = (addr >> 24) & 0xff;
  tr->addr_hi[2] = (addr >> 32) & 0xff;
  tr->addr_hi[3] = (addr >> 40) & 0xff;
  tr->addr_hi[4] = (addr >> 48) & 0xff;
  tr->addr_hi[5] = (addr >> 56) & 0xff;
  tr->e0 = 0xe0;
  tr->addr_lo[0] = ((addr & 0x000f) << 4) | 0x01;
  tr->addr_lo[1] = ((addr & 0x0070) >> 4) | ((addr & 0x070000) >> 11) | ((addr & 0x200000) >> 17);
  tr->addr_lo[2] = ((addr & 0x1f80) >> 5) | ((addr & 0x180000) >> 19);
  tr->addr_lo[3] = ((addr & 0xe000) >> 13) | 0x60;
  grub_memcpy (tr->jump, jump, sizeof (tr->jump));
}

grub_size_t
grub_arch_dl_get_tramp_size (const void *ehdr, unsigned sec)
{
  const Elf_Ehdr *e = ehdr;
  int cnt = 0;
  const Elf_Shdr *s;
  Elf_Word entsize;
  unsigned i;

  /* Find a symbol table.  */
  for (i = 0, s = (Elf_Shdr *) ((char *) e + e->e_shoff);
       i < e->e_shnum;
       i++, s = (Elf_Shdr *) ((char *) s + e->e_shentsize))
    if (s->sh_type == SHT_SYMTAB)
      break;

  if (i == e->e_shnum)
    return 0;

  entsize = s->sh_entsize;

  for (i = 0, s = (Elf_Shdr *) ((char *) e + e->e_shoff);
       i < e->e_shnum;
       i++, s = (Elf_Shdr *) ((char *) s + e->e_shentsize))
    if (s->sh_type == SHT_RELA)
      {
	Elf_Rela *rel, *max;

	if (s->sh_info != sec)
	  continue;
	
	for (rel = (Elf_Rela *) ((char *) e + s->sh_offset),
	       max = rel + s->sh_size / s->sh_entsize;
	     rel < max; rel++)
	    if (ELF_R_TYPE (rel->r_info) == R_IA64_PCREL21B)
	      cnt++;
      }

  return cnt * sizeof (struct ia64_trampoline);
}

/* Relocate symbols.  */
grub_err_t
grub_arch_dl_relocate_symbols (grub_dl_t mod, void *ehdr)
{
  Elf_Ehdr *e = ehdr;
  Elf_Shdr *s;
  Elf_Word entsize;
  unsigned i;
  grub_uint64_t *gp, *gpptr;
  grub_size_t gp_size = 0;

  for (i = 0, s = (Elf_Shdr *) ((char *) e + e->e_shoff);
       i < e->e_shnum;
       i++, s = (Elf_Shdr *) ((char *) s + e->e_shentsize))
    if (s->sh_type == SHT_REL)
      {
	grub_dl_segment_t seg;

	/* Find the target segment.  */
	for (seg = mod->segment; seg; seg = seg->next)
	  if (seg->section == s->sh_info)
	    break;

	if (seg)
	  {
	    Elf_Rel *rel, *max;

	    for (rel = (Elf_Rel *) ((char *) e + s->sh_offset),
		   max = rel + s->sh_size / s->sh_entsize;
		 rel < max;
		 rel++)
		switch (ELF_R_TYPE (rel->r_info))
		  {
		  case R_IA64_LTOFF22X:
		  case R_IA64_LTOFF22:
		    gp_size += 8;
		    break;
		  default: break;
		  }
	  }
      }

  if (gp_size > MASK19)
    return grub_error (GRUB_ERR_OUT_OF_RANGE, "gp too big");

  gpptr = gp = grub_malloc (gp_size);
  if (!gp)
    return grub_errno;
  mod->gp = (char *) gp;

  /* Find a symbol table.  */
  for (i = 0, s = (Elf_Shdr *) ((char *) e + e->e_shoff);
       i < e->e_shnum;
       i++, s = (Elf_Shdr *) ((char *) s + e->e_shentsize))
    if (s->sh_type == SHT_SYMTAB)
      break;

  if (i == e->e_shnum)
    return grub_error (GRUB_ERR_BAD_MODULE, "no symtab found");

  entsize = s->sh_entsize;

  for (i = 0, s = (Elf_Shdr *) ((char *) e + e->e_shoff);
       i < e->e_shnum;
       i++, s = (Elf_Shdr *) ((char *) s + e->e_shentsize))
    if (s->sh_type == SHT_RELA)
      {
	grub_dl_segment_t seg;

	/* Find the target segment.  */
	for (seg = mod->segment; seg; seg = seg->next)
	  if (seg->section == s->sh_info)
	    break;

	if (seg)
	  {
	    Elf_Rela *rel, *max;
	    struct ia64_trampoline *tr;

	    for (rel = (Elf_Rela *) ((char *) e + s->sh_offset),
		   max = rel + s->sh_size / s->sh_entsize;
		 rel < max;
		 rel++)
	      {
		Elf_Word *addr;
		Elf_Sym *sym;
		grub_uint64_t value;
		int slot = 0;

		if (seg->size < (rel->r_offset & ~3))
		  return grub_error (GRUB_ERR_BAD_MODULE,
				     "reloc offset is out of the segment");

		tr = (void *) ((char *) seg->addr + ALIGN_UP (seg->size, GRUB_ARCH_DL_TRAMP_ALIGN));

		if (ELF_R_TYPE (rel->r_info) == R_IA64_PCREL21B)
		  {
		    addr = (Elf_Word *) ((char *) seg->addr + (rel->r_offset & ~3));
		    slot = rel->r_offset & 3;
		  }
		else
		  addr = (Elf_Word *) ((char *) seg->addr + rel->r_offset);
		sym = (Elf_Sym *) ((char *) mod->symtab
				     + entsize * ELF_R_SYM (rel->r_info));

		/* On the PPC the value does not have an explicit
		   addend, add it.  */
		value = sym->st_value + rel->r_addend;
		switch (ELF_R_TYPE (rel->r_info))
		  {
		  case R_IA64_PCREL21B:
		    {
		      grub_uint64_t noff;
		      make_trampoline (tr, value);
		      noff = ((char *) tr - (char *) addr) >> 4;
		      tr++;
		      if (noff & ~MASK19)
			return grub_error (GRUB_ERR_BAD_OS,
					   "trampoline offset too big");
		      add_value_to_slot13_20 (addr, noff, slot);
		    }
		    break;
		  case R_IA64_SEGREL64LSB:
		    *(grub_uint64_t *) addr += value - rel->r_offset;
		    break;
		  case R_IA64_LTOFF22X:
		  case R_IA64_LTOFF22:
		    *gpptr = value;
		    add_value_to_slot13_20 (addr, (gpptr - gp) * sizeof (grub_uint64_t), slot);
		    gpptr++;
		    break;

		    /* We treat LTOFF22X as LTOFF22, so we can ignore LDXMOV.  */
		  case R_IA64_LDXMOV:
		    break;
		  default:
		    return grub_error (GRUB_ERR_NOT_IMPLEMENTED_YET,
				       "this relocation (0x%x) is not implemented yet",
				       ELF_R_TYPE (rel->r_info));
		  }
	      }
	  }
      }

  return GRUB_ERR_NONE;
}
