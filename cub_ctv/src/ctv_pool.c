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
 * sync stats pool - the chain of per-primitive stats, grouped by type and name
 */

#include "ctv.h"

/* -------------------------------------------------------------- pool view */

static const char *pool_type_names[] =
  { "?", "CSECT", "RWLOCK", "RMUTEX", "MUTEX" };

static unsigned char *probe_head = NULL;	/* head chunk image of the probe */
static uint64_t probe_head_addr = 0;

/* I-12: chunk entry count = the candidate N where every entry's type is
 * 0..4, in_use matches the non-NONE count (+-2 for concurrent updates)
 * and 'next' is 0 or points at a type-valid entry */
static int
pool_probe_entries (uint64_t pool_addr)
{
  static const int cands[] = { 256, 128, 64, 512 };
  unsigned int i;

  unsigned char *raw = malloc (CHUNK_BYTES (512));

  if (raw == NULL)
    {
      return -1;
    }
  for (i = 0; i < sizeof (cands) / sizeof (cands[0]); i++)
    {
      int n = cands[i];
      int ok = 0;

      if (mem_read (pool_addr, raw, CHUNK_BYTES (n)) == 0)
	{
	  uint64_t nxt = rd_u64 (raw, CHUNK_OFF_NEXT (n));
	  int32_t in_use = rd_i32 (raw, CHUNK_OFF_INUSE (n));
	  int k, used = 0, types_ok = 1;

	  for (k = 0; k < n && types_ok; k++)
	    {
	      int32_t t = rd_i32 (raw, (size_t) k * STATS_SIZE);

	      types_ok = t >= 0 && t <= ST_TYPE_MUTEX;
	      used += t != 0;
	    }
	  if (types_ok && in_use >= 0 && in_use <= n
	      && used - in_use <= 2 && in_use - used <= 2)
	    {
	      unsigned char head[4];

	      ok = nxt == 0
		|| (mem_read (nxt, head, 4) == 0
		    && rd_i32 (head, 0) >= 0 && rd_i32 (head, 0) <= ST_TYPE_MUTEX);
	    }
	}
      if (ok)
	{
	  /* the head chunk as read here is the walk's first hop */
	  free (probe_head);
	  probe_head = raw;
	  probe_head_addr = pool_addr;
	  return n;
	}
    }
  free (raw);
  return -1;
}


#define GROUP_HASH 4096	/* power of two > 2 * POOL_MAX_GROUPS */

/* walk the chain, grouping entries by (type, name); *truncated = chunk index
 * where the walk stopped early, or -1.  Returns the group count.  nwait is
 * not grouped: the engine counts it only for critical sections (CS table) */
static int
pool_walk (uint64_t pool_addr, int nent, POOL_GROUP * g, int maxg,
	   int *truncated, FILE * dump)
{
  uint64_t chunk = pool_addr;
  int ng = 0, hop = 0, total = 0;
  unsigned short *gh = calloc (GROUP_HASH, sizeof (unsigned short));
  unsigned slot = 0;

  unsigned char *raw = malloc (CHUNK_BYTES (nent));

  *truncated = -1;
  if (raw == NULL || gh == NULL)
    {
      free (raw);
      free (gh);
      *truncated = POOL_NO_MEMORY;
      return 0;
    }
  while (chunk != 0)
    {
      static uint64_t visited[POOL_MAX_CHUNKS];
      int i;

      if (hop >= POOL_MAX_CHUNKS || total >= POOL_MAX_ENTRIES)
	{
	  *truncated = hop;
	  break;
	}
      /* a chunk seen before (torn read, reused memory): stop, or every
       * group would be counted again */
      for (i = 0; i < hop && visited[i] != chunk; i++)
	{
	  ;
	}
      if (i < hop)
	{
	  *truncated = POOL_CHAIN_LOOP;
	  break;
	}
      visited[hop] = chunk;
      if (hop == 0 && probe_head != NULL && probe_head_addr == chunk)
	{
	  memcpy (raw, probe_head, CHUNK_BYTES (nent));
	}
      else if (mem_read (chunk, raw, CHUNK_BYTES (nent)) != 0)
	{
	  *truncated = hop;
	  break;
	}
      for (i = 0; i < nent; i++)
	{
	  STATS_VIEW v;
	  char name[NAME_MAX_LEN];
	  int k;

	  if (stats_decode (raw + (size_t) i * STATS_SIZE, &v) != 0)
	    {
	      continue;
	    }
	  (void) name_of (v.name_ptr, name, sizeof (name));
	  total++;
	  if (dump != NULL)
	    {
	      fprintf (dump, "%s{\"chunk\":%d,\"entry\":%d,\"type\":\"%s\","
		       "\"name\":", total > 1 ? "," : "", hop, i,
		       pool_type_names[v.type]);
	      json_fstr (dump, name);
	      fprintf (dump, ",\"nenter\":%u,\"nwait\":%u,\"nreenter\":%u,"
		       "\"total_ms\":%.3f,\"max_ms\":%.3f}\n", v.nenter,
		       v.nwait, v.nreenter, v.total_ms, v.max_ms);
	    }
	  /* (type, name) -> group through a hash of the walk */
	  {
	    unsigned h = (unsigned) fnv (fnv (FNV_SEED, &v.type, sizeof (v.type)),
					 name, strlen (name));

	    for (h &= GROUP_HASH - 1; gh[h] != 0; h = (h + 1) & (GROUP_HASH - 1))
	      {
		if (g[gh[h] - 1].type == v.type && strcmp (g[gh[h] - 1].name, name) == 0)
		  {
		    break;
		  }
	      }
	    k = gh[h] != 0 ? gh[h] - 1 : ng;
	    slot = h;
	  }
	  if (k == ng)
	    {
	      if (ng >= maxg)
		{
		  if (*truncated == -1)
		    {
		      *truncated = POOL_GROUPS_CAPPED;	/* chain fine, groups capped */
		    }
		  continue;
		}
	      memset (&g[ng], 0, sizeof (g[ng]));
	      g[ng].type = v.type;
	      snprintf (g[ng].name, sizeof (g[ng].name), "%s", name);
	      gh[slot] = ++ng;
	    }
	  g[k].cnt++;
	  g[k].nenter += v.nenter;
	  if (v.max_ms > g[k].max_ms)
	    {
	      g[k].max_ms = v.max_ms;
	    }
	}
      chunk = rd_u64 (raw, CHUNK_OFF_NEXT (nent));
      hop++;
    }
  free (raw);
  free (gh);
  return ng;
}

/* CSECT entries are the CS table itself and are not repeated here */
void
pool_print_groups (const POOL_GROUP * g, int ng, int truncated)
{
  int t;

  for (t = ST_TYPE_CSECT + 1; t <= ST_TYPE_MUTEX; t++)
    {
      int k, tt = 0, shown = 0;

      for (k = 0; k < ng; k++)
	{
	  tt += g[k].type == t ? g[k].cnt : 0;
	}
      for (k = 0; k < ng; k++)
	{
	  if (g[k].type != t)
	    {
	      continue;
	    }
	  if (!shown)
	    {
	      printf ("\n%s", clr (CLR_HEAD));
	      printf (M ("[pool] %s: %d entries (grouped by name)",
			 "[풀] %s: %d엔트리 (이름별 그룹)"), pool_type_names[t], tt);
	      printf ("%s\n%s  %-36s %5s %12s %10s%s\n", clr (CLR_OFF),
		      clr (CLR_HEAD), "name", "n", "nenter", "max_ms",
		      clr (CLR_OFF));
	      shown = 1;
	    }
	  printf ("  %-36s %5d %12lu %10.3f\n", g[k].name, g[k].cnt,
		  (unsigned long) g[k].nenter, g[k].max_ms);
	}
    }
  if (truncated >= 0)
    {
      printf (M ("  (pool chain truncated at chunk %d - counts incomplete)\n",
		 "  (풀 체인이 청크 %d에서 끊김 - 집계 불완전)\n"), truncated);
    }
  else if (truncated == POOL_NO_MEMORY)
    {
      printf (M ("  (pool not grouped: out of memory in cub_ctv)\n",
		 "  (풀 집계 생략: cub_ctv 메모리 부족)\n"));
    }
  else if (truncated == POOL_CHAIN_LOOP)
    {
      printf (M ("  (pool chain came back to a chunk already read - stopped there)\n",
		 "  (풀 체인이 이미 읽은 청크로 돌아옴 - 그 지점에서 멈춤)\n"));
    }
  else if (truncated == POOL_GROUPS_CAPPED)
    {
      printf (M ("  (more than %d distinct names - the rest not grouped)\n",
		 "  (서로 다른 이름이 %d개를 넘음 - 나머지는 묶지 않음)\n"),
	      POOL_MAX_GROUPS);
    }
}

/* group the pool by (type, name); -1 when the pool does not validate */
int
pool_collect (uint64_t pool_addr, POOL_GROUP * g, int maxg, int *ng,
	      int *truncated)
{
  int nent = pool_probe_entries (pool_addr);

  if (nent < 0)
    {
      return -1;
    }
  *ng = pool_walk (pool_addr, nent, g, maxg, truncated, NULL);
  return 0;
}

/* every pool entry as JSON array elements (detail dump) */
int
pool_dump (FILE * f, uint64_t pool_addr)
{
  int nent = pool_probe_entries (pool_addr);

  if (nent < 0)
    {
      view.pool_state = -1;
      return -1;
    }
  /* one walk: every entry to the dump, the groups to the frame */
  fprintf (f, "\"pool_chunk_entries\":%d,\"pool\":[\n", nent);
  view.npool = pool_walk (pool_addr, nent, view.pool, POOL_MAX_GROUPS,
			  &view.pool_truncated, f);
  view.pool_state = 1;
  /* the chunk where the chain was cut (-1: not cut), and how the walk
   * ended in words: the sentinels are not chunk numbers */
  {
    int t = view.pool_truncated;

    fprintf (f, "],\"pool_truncated_at_chunk\":%d,\"pool_walk\":\"%s\"",
	     t >= 0 ? t : -1, t >= 0 ? "chain_cut" : t == POOL_GROUPS_CAPPED
	     ? "groups_capped" : t == POOL_NO_MEMORY ? "no_memory"
	     : t == POOL_CHAIN_LOOP ? "chain_loop" : "complete");
  }
  return 0;
}
