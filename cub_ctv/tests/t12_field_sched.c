/*
 * t12: the rules of a probed field (FIELD) and of the retry schedule
 * (SCHED), without a server.
 */
#include "ctv.h"

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf ("t12 FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static int want_off, want_weak, calls;
static int
pass (const FIELD *f, const unsigned char *img, int ignore, int *weak)
{
  (void) f;
  (void) img;
  calls++;
  *weak = want_weak;
  /* offset 40 passes; it is "rejected" unless ignored */
  return want_off == 40 && !ignore ? -1 : want_off;
}

int
main (void)
{
  FIELD f;
  SCHED s;
  int k, gaps[10];
  double last = 0;

  memset (&f, 0, sizeof (f));
  field_lock (&f, 40, 1);
  CHECK (f.state == FIELD_WEAK && f.off == 40);
  field_lock (&f, 40, 0);
  CHECK (f.state == FIELD_SET);
  /* a drop needs 4 misses outnumbering hits */
  for (k = 0; k < 3; k++)
    CHECK (!field_sight (&f, 0));
  CHECK (field_sight (&f, 0));
  field_reject (&f);
  CHECK (f.state == FIELD_NONE && f.nrejected == 1 && field_skips (&f, 40, 0));
  CHECK (!field_skips (&f, 40, 1));
  /* the only passing offset was rejected: one more chance, list cleared */
  want_off = 40; calls = 0;
  CHECK (field_probe (&f, pass, NULL) && f.off == 40 && f.nrejected == 0 && calls == 2);
  /* a held field is not retried with the rejected ones */
  field_reject (&f);
  field_lock (&f, 8, 0);
  calls = 0;
  CHECK (!field_probe (&f, pass, NULL) && calls == 1 && f.off == 8);
  /* the rejected list keeps the newest FIELD_REJECTS */
  for (k = 0; k < FIELD_REJECTS + 3; k++)
    {
      field_lock (&f, 100 + k, 0);
      field_reject (&f);
    }
  CHECK (f.nrejected == FIELD_REJECTS && f.rejected[0] == 103);
  /* a new offset restarts the tally */
  field_lock (&f, 1, 0); (void) field_sight (&f, 0);
  field_lock (&f, 2, 0);
  CHECK (f.misses == 0);

  /* schedule: 2,4,..,64 then the slow gap */
  sched_start (&s, 1000);
  CHECK (s.at == 1000 + SCHED_FIRST && !sched_due (&s, 1001) && sched_due (&s, 1002));
  for (k = 0; k < SCHED_FAST_ROUNDS + 2; k++)
    {
      sched_ran (&s, 2000);
      gaps[k] = s.gap;
    }
  CHECK (gaps[0] == 4 && gaps[4] == 64 && gaps[5] == 64);
  CHECK (gaps[SCHED_FAST_ROUNDS] == SCHED_SLOW && sched_slow (&s));
  sched_sooner (&s, 60);
  CHECK (s.at == 2060);
  sched_restart (&s);
  CHECK (!sched_slow (&s) && sched_due (&s, 0));
  CHECK (every (&last, 1.0, 1.0) && !every (&last, 1.5, 1.0) && every (&last, 2.0, 1.0));

  if (fails == 0)
    printf ("t12 PASS: field lock/sight/reject/retry and schedule fast/slow/sooner/restart rules\n");
  return fails != 0;
}
