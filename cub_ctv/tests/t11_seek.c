/*
 * t11: every frame of a record reached by view_replay_goto (backward, then
 * at random) must carry the identities a sequential read gives it.
 */
#include "ctv.h"

static char *
idents (void)
{
  size_t cap = 1 << 16, o = 0;
  char *b = malloc (cap);
  int i;

  b[0] = '\0';
  for (i = 0; i < view.nident && o + 512 < cap; i++)
    {
      o += (size_t) snprintf (b + o, cap - o, "%d/%d/%s/%s/%s;", view.ident[i].idx,
			      view.ident[i].trid, view.ident[i].id.user,
			      view.ident[i].id.program, view.ident[i].id.host);
    }
  return b;
}

int
main (int argc, char **argv)
{
  VIEW_HDR h;
  char *seq[1024];
  int n = 0, i, bad = 0;

  if (argc != 2)
    {
      return 2;
    }
  view_replay_open (argv[1], &h);
  while (n < 1024 && view_replay_next () == 0)
    {
      seq[n++] = idents ();
    }
  view_replay_open (argv[1], &h);
  if (view_replay_index () != n)
    {
      printf ("t11 seek: index count differs\n");
      return 1;
    }
  srand (11);
  for (i = 0; i < 3 * n; i++)
    {
      int k = i < n ? n - 1 - i : rand () % n;
      char *t;

      if (view_replay_goto (k) != 0)
	{
	  printf ("t11 seek: goto %d failed\n", k);
	  return 1;
	}
      t = idents ();
      bad += strcmp (t, seq[k]) != 0;
      free (t);
    }
  for (i = 0; i < n; i++)
    {
      free (seq[i]);
    }
  if (bad)
    {
      printf ("t11 seek: %d of %d seeks differ\n", bad, 3 * n);
    }
  return bad != 0;
}
