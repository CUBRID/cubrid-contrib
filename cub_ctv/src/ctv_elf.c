/*
 * Copyright 2008 Search Solution Corporation
 * Copyright 2016 CUBRID Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 */

/*
 * ELF - the mapped libcubrid.so: its base in the target and symbol lookups
 */

#include "ctv.h"

/* --------------------------------------------------------------- ELF side */

/* the library file, mapped read-only once and shared by every lookup */
static struct
{
  char path[4096];
  unsigned char *m;
  uint64_t size;
} elf_file;

static int
elf_map (const char *lib)
{
  int fd;
  struct stat st;
  void *m;

  if (elf_file.m != NULL && strcmp (elf_file.path, lib) == 0)
    {
      return ELF_OK;
    }
  if (elf_file.m != NULL)
    {
      munmap (elf_file.m, (size_t) elf_file.size);
      elf_file.m = NULL;
    }
  fd = open (lib, O_RDONLY);
  if (fd < 0)
    {
      return ELF_NOFILE;
    }
  if (fstat (fd, &st) != 0 || (size_t) st.st_size < sizeof (Elf64_Ehdr))
    {
      close (fd);
      return ELF_BAD;
    }
  m = mmap (NULL, (size_t) st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close (fd);
  if (m == MAP_FAILED)
    {
      return ELF_BAD;
    }
  snprintf (elf_file.path, sizeof (elf_file.path), "%s", lib);
  elf_file.m = m;
  elf_file.size = (uint64_t) st.st_size;
  return ELF_OK;
}

/* link-time address of a defined symbol + vaddr of the PT_LOAD at file
 * offset 0 (load-bias computation); .dynsym and .symtab are both scanned.
 * I-13: every table offset is checked against the file size. */
int
elf_lookup (const char *lib, const char *want, uint64_t * value,
	    uint64_t * load0_vaddr)
{
  unsigned char *m;
  Elf64_Ehdr *eh;
  Elf64_Phdr *ph;
  Elf64_Shdr *sh;
  uint64_t fsz;
  int i, found = 0, rc = elf_map (lib);

  if (rc != ELF_OK)
    {
      return rc;
    }
  m = elf_file.m;
  eh = (Elf64_Ehdr *) m;
  fsz = elf_file.size;
  if (memcmp (eh->e_ident, ELFMAG, SELFMAG) != 0
      || eh->e_ident[EI_CLASS] != ELFCLASS64
      || eh->e_phentsize != sizeof (Elf64_Phdr)
      || eh->e_shentsize != sizeof (Elf64_Shdr)
      || eh->e_phoff > fsz
      || (uint64_t) eh->e_phnum * sizeof (Elf64_Phdr) > fsz - eh->e_phoff
      || eh->e_shoff > fsz
      || (uint64_t) eh->e_shnum * sizeof (Elf64_Shdr) > fsz - eh->e_shoff)
    {
      return ELF_BAD;
    }

  *load0_vaddr = 0;
  ph = (Elf64_Phdr *) (m + eh->e_phoff);
  for (i = 0; i < eh->e_phnum; i++)
    {
      if (ph[i].p_type == PT_LOAD && ph[i].p_offset == 0)
	{
	  *load0_vaddr = ph[i].p_vaddr;
	  break;
	}
    }

  sh = (Elf64_Shdr *) (m + eh->e_shoff);
  for (i = 0; i < eh->e_shnum && !found; i++)
    {
      Elf64_Sym *sym;
      const char *strtab;
      uint64_t nsym, k, stroff, strsz;

      if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM)
	{
	  continue;
	}
      if (sh[i].sh_entsize != sizeof (Elf64_Sym) || sh[i].sh_link >= eh->e_shnum
	  || sh[i].sh_offset > fsz || sh[i].sh_size > fsz - sh[i].sh_offset)
	{
	  continue;
	}
      stroff = sh[sh[i].sh_link].sh_offset;
      strsz = sh[sh[i].sh_link].sh_size;
      if (stroff > fsz || strsz > fsz - stroff || strsz == 0)
	{
	  continue;
	}
      sym = (Elf64_Sym *) (m + sh[i].sh_offset);
      strtab = (const char *) (m + stroff);
      nsym = sh[i].sh_size / sh[i].sh_entsize;
      for (k = 0; k < nsym; k++)
	{
	  if (sym[k].st_shndx == SHN_UNDEF || sym[k].st_name == 0
	      || sym[k].st_name >= strsz)
	    {
	      continue;
	    }
	  if (strncmp (strtab + sym[k].st_name, want,
		       strsz - sym[k].st_name) == 0
	      && strlen (want) < strsz - sym[k].st_name)
	    {
	      *value = sym[k].st_value;
	      found = 1;
	      break;
	    }
	}
    }
  return found ? ELF_OK : ELF_NOSYM;
}

/* lowest mapping of libcubrid.so (basename-prefix matched) and its path */
int
find_lib_base (int pid, uint64_t * base, char *path, size_t pathn,
	       char *range, size_t rangen)
{
  char mp[64], line[PATH_MAX + 512];	/* a maps line: fields + a full path */
  FILE *f;
  uint64_t best = 0, dbest = 0;
  int got = 0, deleted = 0;
  char dpath[PATH_MAX + 512] = "", drange[64] = "";

  snprintf (mp, sizeof (mp), "/proc/%d/maps", pid);
  f = fopen (mp, "r");
  if (f == NULL)
    {
      return LIB_NOMAPS;
    }
  range[0] = '\0';
  while (fgets (line, sizeof (line), f) != NULL)
    {
      uint64_t start;
      char *p, *bn;
      size_t len;
      int fld, is_del;

      if (strchr (line, '\n') == NULL && !feof (f))
	{
	  int ch;

	  while ((ch = fgetc (f)) != EOF && ch != '\n')
	    {
	      ;			/* longer than any path: not ours, skip it whole */
	    }
	  continue;
	}
      if (strstr (line, "libcubrid.so") == NULL
	  || sscanf (line, "%lx", &start) != 1)
	{
	  continue;
	}
      /* the path is the 6th maps field and may contain spaces */
      p = line;
      for (fld = 0; fld < 5; fld++)
	{
	  while (*p != '\0' && *p != ' ' && *p != '\t')
	    {
	      p++;
	    }
	  while (*p == ' ' || *p == '\t')
	    {
	      p++;
	    }
	}
      if (*p != '/')
	{
	  continue;
	}
      len = strlen (p);
      while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == ' '))
	{
	  p[--len] = '\0';
	}
      is_del = len > 10 && strcmp (p + len - 10, " (deleted)") == 0;
      if (is_del)
	{
	  p[len - 10] = '\0';	/* the path as it was mapped */
	}
      bn = strrchr (p, '/');
      bn = (bn != NULL) ? bn + 1 : p;
      if (strncmp (bn, "libcubrid.so", 12) != 0
	  || (bn[12] != '\0' && bn[12] != '.'))
	{
	  continue;
	}
      if (is_del)
	{
	  /* replaced on disk: still readable through map_files (lowest
	   * mapping = ELF header) */
	  if (!deleted || start < dbest)
	    {
	      dbest = start;
	      snprintf (drange, sizeof (drange), "%.*s",
			(int) strcspn (line, " "), line);
	      snprintf (dpath, sizeof (dpath), "%s", p);
	    }
	  deleted = 1;
	  continue;
	}
      if (!got || start < best)
	{
	  best = start;
	  got = 1;
	  snprintf (path, pathn, "%s", p);
	  snprintf (range, rangen, "%.*s", (int) strcspn (line, " "), line);
	}
    }
  fclose (f);
  if (!got)
    {
      if (deleted)
	{
	  *base = dbest;
	  snprintf (path, pathn, "%s", dpath);
	  snprintf (range, rangen, "%s", drange);
	}
      return deleted ? LIB_DELETED : LIB_NONE;
    }
  *base = best;
  return LIB_OK;
}
