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
 * field - the state of one probed field (an invariant's offset): not
 * known, provisional (weak rule) or settled; the runtime anchor's tally;
 * the offsets the anchor rejected.  Every change of a field's state goes
 * through these functions.
 */

#include "ctv.h"

/* lock the field at off; a new offset starts its runtime tally over */
void
field_lock (FIELD * f, int off, int weak)
{
  if (f->state == FIELD_NONE || f->off != off)
    {
      f->hits = f->misses = 0;
    }
  f->off = off;
  f->state = weak ? FIELD_WEAK : FIELD_SET;
}

/* not known (to be found again); the rejected offsets stay */
void
field_clear (FIELD * f)
{
  f->state = FIELD_NONE;
}

/* the runtime anchor drops the field: its offset is remembered (the oldest
 * of FIELD_REJECTS goes) so the next probe tries the others first */
void
field_reject (FIELD * f)
{
  if (f->state != FIELD_NONE)
    {
      if (f->nrejected == FIELD_REJECTS)
	{
	  memmove (f->rejected, f->rejected + 1, (FIELD_REJECTS - 1) * sizeof (int));
	  f->nrejected--;
	}
      f->rejected[f->nrejected++] = f->off;
    }
  f->state = FIELD_NONE;
}

/* off was rejected before (and the rejections are not ignored) */
int
field_skips (const FIELD * f, int off, int ignore)
{
  int k;

  for (k = 0; !ignore && k < f->nrejected; k++)
    {
      if (f->rejected[k] == off)
	{
	  return 1;
	}
    }
  return 0;
}

/* one runtime sighting: 1 when the field should be dropped.  Old sightings
 * weigh less (halved at 64); a drop needs 4 misses outnumbering the hits */
int
field_sight (FIELD * f, int ok)
{
  if (ok)
    {
      f->hits++;
    }
  else
    {
      f->misses++;
    }
  if (f->hits + f->misses >= 64)
    {
      f->hits /= 2;
      f->misses /= 2;
    }
  return f->misses >= 4 && f->misses > f->hits;
}

/* probe the field with pass (offset or -1, *weak for the provisional rule);
 * rejected offsets are skipped, unless nothing else passes and the field is
 * not held: then they get one more chance (a rejection may come from a
 * head read just after it was served), and a hit clears the list */
int
field_probe (FIELD * f, int (*pass) (const FIELD *, const unsigned char *, int, int *),
	     const unsigned char *img)
{
  int weak = 0, off = pass (f, img, 0, &weak);

  if (off < 0 && f->state == FIELD_NONE && f->nrejected > 0)
    {
      off = pass (f, img, 1, &weak);
      if (off >= 0)
	{
	  f->nrejected = 0;
	}
    }
  if (off >= 0)
    {
      field_lock (f, off, weak);
    }
  return off >= 0;
}
