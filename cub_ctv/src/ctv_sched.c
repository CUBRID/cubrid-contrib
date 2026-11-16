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
 * sched - when to retry something costly: fast rounds with a doubling gap,
 * then a slow fixed gap; a restart returns to the fast rounds, and a change
 * can bring the next try forward (rate-limited).  Also a plain minimum
 * interval gate.
 */

#include "ctv.h"

/* the first try was just made: the next one after the first gap */
void
sched_start (SCHED * s, time_t now)
{
  s->rounds = 0;
  s->gap = SCHED_FIRST;
  s->last = now;
  s->at = now + s->gap;
}

int
sched_due (const SCHED * s, time_t now)
{
  return now >= s->at;
}

/* a try is being made now: the gap doubles up to SCHED_FAST_MAX for
 * SCHED_FAST_ROUNDS tries, then stays at SCHED_SLOW */
void
sched_ran (SCHED * s, time_t now)
{
  s->gap = ++s->rounds > SCHED_FAST_ROUNDS ? SCHED_SLOW
    : s->gap == 0 ? SCHED_FIRST : s->gap < SCHED_FAST_MAX ? s->gap * 2 : SCHED_FAST_MAX;
  s->at = now + s->gap;
  s->last = now;
}

/* back to the fast rounds, due at once */
void
sched_restart (SCHED * s)
{
  s->rounds = s->gap = 0;
  s->at = 0;
}

/* past the fast rounds */
int
sched_slow (const SCHED * s)
{
  return s->rounds > SCHED_FAST_ROUNDS;
}

/* something changed: the next try no later than min_gap after the last */
void
sched_sooner (SCHED * s, int min_gap)
{
  if (s->at > s->last + min_gap)
    {
      s->at = s->last + min_gap;
    }
}

/* at most once per gap: 1 (and *last = now) when gap has passed */
int
every (double *last, double now, double gap)
{
  if (now - *last < gap)
    {
      return 0;
    }
  *last = now;
  return 1;
}
