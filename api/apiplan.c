/*
 * Copyright (c) 2003, 2007-14 Matteo Frigo
 * Copyright (c) 2003, 2007-14 Massachusetts Institute of Technology
 * Copyright (C) 2019-2021, Advanced Micro Devices, Inc. All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "api/api.h"

#ifdef AMD_APP_OPT_LAYER
#include "kernel/ifftw.h"
#include "dft/dft.h"
#include "rdft/rdft.h"

static int wisdom_one_time_read = 0;
#endif
static planner_hook_t before_planner_hook = 0, after_planner_hook = 0;

void X(set_planner_hooks)(planner_hook_t before, planner_hook_t after)
{
     before_planner_hook = before;
     after_planner_hook = after;
}

#ifdef AMD_TOP_N_PLANNER
plan *plans[AMD_OPT_TOP_N];
static int find_lowcost_plan()
{
    int i, lowcost, lowcost_id;
    lowcost = plans[0]->pcost;
    lowcost_id = 0;

    for (i = 1; i < AMD_OPT_TOP_N; i++) {
         if (plans[i]->pcost < lowcost) {
              lowcost = plans[i]->pcost;
              lowcost_id = i;
         }
    }
    return lowcost_id;
}
#endif

static plan *mkplan0(planner *plnr, unsigned flags,
		     const problem *prb, unsigned hash_info,
		     wisdom_state_t wisdom_state)
{
#ifdef AMD_TOP_N_PLANNER
     static int lowcost_idx;	/* to hold the index of the plan which has the least pcost among the top N plans*/     
/* map API flags into FFTW flags */
     X(mapflags)(plnr, flags);

     plnr->flags.hash_info = hash_info;
     plnr->wisdom_state = wisdom_state;
     
     /* create plan */

     if (AMD_OPT_TOP_N > 1) {
          if (wisp_set == 1) {
               for (int pln_idx = 0; pln_idx < AMD_OPT_TOP_N ; pln_idx ++) {
                    plnr->index = pln_idx;
	            plans[pln_idx] = plnr->adt->mkplan(plnr, prb);
               }
               lowcost_idx = find_lowcost_plan(plans);
               return plans[lowcost_idx];
          }
          else {        
               for (int pln_idx = 0; pln_idx < AMD_OPT_TOP_N ; pln_idx ++) {
                    plnr->index = pln_idx;
	            plans[pln_idx] = plnr->adt->mkplan(plnr, prb);
               }	   
	       return plans[0];
          }
     }   
     else {
          plnr->index = 0;
          return plnr->adt->mkplan(plnr, prb);
     }	
#else	
     /* map API flags into FFTW flags */
     X(mapflags)(plnr, flags);

     plnr->flags.hash_info = hash_info;
     plnr->wisdom_state = wisdom_state;

     /* create plan */
     return plnr->adt->mkplan(plnr, prb);
#endif     
}

static unsigned force_estimator(unsigned flags)
{
     flags &= ~(FFTW_MEASURE | FFTW_PATIENT | FFTW_EXHAUSTIVE);
     return (flags | FFTW_ESTIMATE);
}

static plan *mkplan(planner *plnr, unsigned flags,
		    const problem *prb, unsigned hash_info)
{
     plan *pln;
     
     pln = mkplan0(plnr, flags, prb, hash_info, WISDOM_NORMAL);

     if (plnr->wisdom_state == WISDOM_NORMAL && !pln) {
	  /* maybe the planner failed because of inconsistent wisdom;
	     plan again ignoring infeasible wisdom */
	  pln = mkplan0(plnr, force_estimator(flags), prb,
			hash_info, WISDOM_IGNORE_INFEASIBLE);
     }

     if (plnr->wisdom_state == WISDOM_IS_BOGUS) {
	  /* if the planner detected a wisdom inconsistency,
	     forget all wisdom and plan again */
	  plnr->adt->forget(plnr, FORGET_EVERYTHING);

	  A(!pln);
	  pln = mkplan0(plnr, flags, prb, hash_info, WISDOM_NORMAL);

	  if (plnr->wisdom_state == WISDOM_IS_BOGUS) {
	       /* if it still fails, plan without wisdom */
	       plnr->adt->forget(plnr, FORGET_EVERYTHING);

	       A(!pln);
	       pln = mkplan0(plnr, force_estimator(flags),
			     prb, hash_info, WISDOM_IGNORE_ALL);
	  }
     }

     return pln;
}

#ifdef AMD_APP_OPT_LAYER
/** AMD's application optimization layer - Starts
  *  It uses a separate data structure "app_layer_data" to create separate planner memory region
  *  and save/restore the application input and output pointers.
  *  It uses new functions to create and destroy the separate planner memory region, 
  *  set planning mode to OPATIENT and OWISDOM, and save/restore application input and output pointers.
  */
typedef struct app_layer_data_s
{
	R *inPtr;
	R *outPtr;
	R *ri;
	R *ii;
	R *ro;
	R *io;
} app_layer_data;

static int create_amd_app_layer(int sign, unsigned *flags, problem *prb, app_layer_data *app_layer)
{
	int isz, osz, inplace = 0;
	int align_bytes = 0, in_alignment = 1, cur_alloc_alignment = 1, iaddr_changed = 0, oaddr_changed = 0;

	*flags &= ~(FFTW_ESTIMATE | FFTW_MEASURE | FFTW_PATIENT | FFTW_EXHAUSTIVE);
	*flags |= FFTW_PATIENT;
	if (wisdom_one_time_read == 0)
	{
		if (!X(import_wisdom_from_filename)("wis.dat"))
		{
			//fprintf(stderr, "apiplan: ERROR reading wisdom wis.dat\n");
		}
#ifdef AMD_APP_OPT_GENERATE_WISDOM
		wisdom_one_time_read = 1;
#endif
	}

	if(prb->adt->problem_kind == PROBLEM_DFT)
	{
		problem_dft *pdft = (problem_dft *) prb;
		isz = 1;
		osz = 1;
		if (FINITE_RNK(pdft->sz->rnk)) 
		{
			for (int i = 0; i < pdft->sz->rnk; ++i) 
			{
				const iodim *q = pdft->sz->dims + i;
				isz *= (q->n);
				osz *= (q->n);
			}
		}
		if (FINITE_RNK(pdft->vecsz->rnk)) 
		{
			for (int i = 0; i < pdft->vecsz->rnk; ++i) 
			{
				const iodim *q = pdft->vecsz->dims + i;
				isz *= (q->n);
				osz *= (q->n);
			}
		}
#ifdef AMD_APP_LAYER_API_LOGS
		printf("start-App: %d*%d*%d*%d\n", pdft->sz->rnk, pdft->vecsz->rnk, pdft->sz->dims->n, pdft->vecsz->dims->n);
		printf("start-App: %x, %x; %x, %x\n", pdft->ri, pdft->ii, pdft->ro, pdft->io);
#endif
		app_layer->inPtr = ((sign == FFT_SIGN) ? pdft->ri : pdft->ii);
		align_bytes = (2 * sizeof(R))-1;
		if (((ptrdiff_t)(app_layer->inPtr)) & align_bytes)
			in_alignment = 0;

		app_layer->ri = pdft->ri;
		app_layer->ii = pdft->ii;
		inplace = (pdft->ri == pdft->ro);
		app_layer->inPtr = (R *) malloc((isz * sizeof(R) * 2) + sizeof(R));

		if (((ptrdiff_t)(app_layer->inPtr)) & align_bytes)
			cur_alloc_alignment = 0;
		if ((in_alignment == 0 && cur_alloc_alignment == 1) ||
				(in_alignment == 1 && cur_alloc_alignment == 0))
		{
			iaddr_changed = 1;
		}

		if (sign == FFT_SIGN) 
		{
			pdft->ri = app_layer->inPtr + iaddr_changed;
			pdft->ii = pdft->ri + 1;
		}
		else
		{
			pdft->ii = app_layer->inPtr + iaddr_changed;
			pdft->ri = pdft->ii + 1;
		}

		if (inplace)
		{
			pdft->ro = pdft->ri;
			pdft->io = pdft->ii;
		}
		else 
		{
#ifdef AMD_APP_OPT_OUT_BUFFER_MEM
			app_layer->ro = pdft->ro;
			app_layer->io = pdft->io;
			in_alignment = 1;
			cur_alloc_alignment = 1;
			app_layer->outPtr = ((sign == FFT_SIGN) ? pdft->ro : pdft->io);
			if (((ptrdiff_t)(app_layer->outPtr)) & align_bytes)
				in_alignment = 0;
			app_layer->outPtr = (R *) malloc((osz * sizeof(R) * 2) + sizeof(R));

			if (((ptrdiff_t)(app_layer->outPtr)) & align_bytes)
				cur_alloc_alignment = 0;
			if ((in_alignment == 0 && cur_alloc_alignment == 1) ||
					(in_alignment == 1 && cur_alloc_alignment == 0))
			{
				oaddr_changed = 1;
			}

			if (sign == FFT_SIGN) 
			{
				pdft->ro = app_layer->outPtr + oaddr_changed;
				pdft->io = pdft->ro + 1;
			}
			else
			{
				pdft->io = app_layer->outPtr + oaddr_changed;
				pdft->ro = pdft->io + 1;
			}
#endif
		}
#ifdef AMD_APP_LAYER_API_LOGS		
		printf("start-FFTW: (in-place:%d), %x, %x; %x, %x\n", inplace, pdft->ri, pdft->ii, pdft->ro, pdft->io);
		printf("start-FFTW: %x, %x; %x, %x\n", app_layer->ri, app_layer->ii, app_layer->ro, app_layer->io);
		printf("FFTW app_layer: %x, %x; %x, %x\n", app_layer->ri, app_layer->ii, app_layer->ro, app_layer->io);
#endif
	}
	else
	{
		fprintf(stderr, "apiplan: UNSUPPORTED problem type/kind\n");
		return -1;
	}
	return 0;
}

static void destroy_amd_app_layer(problem *prb, app_layer_data *app_layer)
{
	int inplace = 0;
	problem_dft *pdft = (problem_dft *) prb;

	if (wisdom_one_time_read == 0)
	{
#ifdef AMD_APP_OPT_GENERATE_WISDOM
		wisdom_one_time_read = 1;
#endif
		X(export_wisdom_to_filename)("wis.dat");
	}

	inplace = (pdft->ri == pdft->ro);
	if(prb->adt->problem_kind == PROBLEM_DFT)
	{
		problem_dft *pdft = (problem_dft *) prb;
		free(app_layer->inPtr);
		pdft->ri = app_layer->ri;
		pdft->ii = app_layer->ii;
		if (inplace)
		{
			pdft->ro = app_layer->ri;
			pdft->io = app_layer->ii;
		}
		else
		{
#ifdef AMD_APP_OPT_OUT_BUFFER_MEM
			free(app_layer->outPtr);
			pdft->ro = app_layer->ro;
			pdft->io = app_layer->io;
#endif
		}
#ifdef AMD_APP_LAYER_API_LOGS
		printf("end-App: %x, %x; %x, %x\n", pdft->ri, pdft->ii, pdft->ro, pdft->io);
#endif
	}
}
/** AMD's application optimization layer - Ends 
 */
#endif

apiplan *X(mkapiplan)(int sign, unsigned flags, problem *prb)
{
     apiplan *p = 0;
     plan *pln;
     unsigned flags_used_for_planning;
     planner *plnr;
     static const unsigned int pats[] = {FFTW_ESTIMATE, FFTW_MEASURE,
                                         FFTW_PATIENT, FFTW_EXHAUSTIVE};
     int pat, pat_max;
     double pcost = 0;
	 
#ifdef AMD_APP_OPT_LAYER
     app_layer_data app_layer;
     if (create_amd_app_layer(sign, &flags, prb, &app_layer))
	     return NULL;
#endif
     if (before_planner_hook)
          before_planner_hook();
     
     plnr = X(the_planner)();

     if (flags & FFTW_WISDOM_ONLY) {
	  /* Special mode that returns a plan only if wisdom is present,
	     and returns 0 otherwise.  This is now documented in the manual,
	     as a way to detect whether wisdom is available for a problem. */
	  flags_used_for_planning = flags;
	  pln = mkplan0(plnr, flags, prb, 0, WISDOM_ONLY);
     } else {
	  pat_max = flags & FFTW_ESTIMATE ? 0 :
	       (flags & FFTW_EXHAUSTIVE ? 3 :
		(flags & FFTW_PATIENT ? 2 : 1));
	  pat = plnr->timelimit >= 0 ? 0 : pat_max;

	  flags &= ~(FFTW_ESTIMATE | FFTW_MEASURE |
		     FFTW_PATIENT | FFTW_EXHAUSTIVE);

	  plnr->start_time = X(get_crude_time)();

	  /* plan at incrementally increasing patience until we run
	     out of time */
	  for (pln = 0, flags_used_for_planning = 0; pat <= pat_max; ++pat) {
	       plan *pln1;
	       unsigned tmpflags = flags | pats[pat];
	       pln1 = mkplan(plnr, tmpflags, prb, 0u);

	       if (!pln1) {
		    /* don't bother continuing if planner failed or timed out */
		    A(!pln || plnr->timed_out);
		    break;
	       }

	       X(plan_destroy_internal)(pln);
	       pln = pln1;
	       flags_used_for_planning = tmpflags;
	       pcost = pln->pcost;
	  }
     }

     if (pln) {
	  /* build apiplan */
	  p = (apiplan *) MALLOC(sizeof(apiplan), PLANS);
	  p->prb = prb;
	  p->sign = sign; /* cache for execute_dft */

	  /* re-create plan from wisdom, adding blessing */
	  p->pln = mkplan(plnr, flags_used_for_planning, prb, BLESSING);

	  /* record pcost from most recent measurement for use in X(cost) */
	  p->pln->pcost = pcost;

	  if (sizeof(trigreal) > sizeof(R)) {
	       /* this is probably faster, and we have enough trigreal
		  bits to maintain accuracy */
	       X(plan_awake)(p->pln, AWAKE_SQRTN_TABLE);
	  } else {
	       /* more accurate */
	       X(plan_awake)(p->pln, AWAKE_SINCOS);
	  }

	  /* we don't use pln for p->pln, above, since by re-creating the
	     plan we might use more patient wisdom from a timed-out mkplan */
	  X(plan_destroy_internal)(pln);
     } else
	  X(problem_destroy)(prb);

     /* discard all information not necessary to reconstruct the plan */
     plnr->adt->forget(plnr, FORGET_ACCURSED);

#ifdef FFTW_RANDOM_ESTIMATOR
     X(random_estimate_seed)++; /* subsequent "random" plans are distinct */
#endif

     if (after_planner_hook)
          after_planner_hook();
     
#ifdef AMD_APP_OPT_LAYER
     destroy_amd_app_layer(prb, &app_layer);
#endif
     return p;
}

#ifdef AMD_OPT_PREFER_256BIT_FPU
apiplan *X(mkapiplan_ex)(int sign, unsigned flags, int n, problem *prb)
{
     apiplan *p = 0;
     plan *pln;
     unsigned flags_used_for_planning;
     planner *plnr;
     static const unsigned int pats[] = {FFTW_ESTIMATE, FFTW_MEASURE,
                                         FFTW_PATIENT, FFTW_EXHAUSTIVE};
     int pat, pat_max;
     double pcost = 0;
	 
#ifdef AMD_APP_OPT_LAYER
     app_layer_data app_layer;
     if (create_amd_app_layer(sign, &flags, prb, &app_layer))
	     return NULL;
#endif
     if (before_planner_hook)
          before_planner_hook();
     
     plnr = X(the_planner_ex)(n);

     if (flags & FFTW_WISDOM_ONLY) {
	  /* Special mode that returns a plan only if wisdom is present,
	     and returns 0 otherwise.  This is now documented in the manual,
	     as a way to detect whether wisdom is available for a problem. */
	  flags_used_for_planning = flags;
	  pln = mkplan0(plnr, flags, prb, 0, WISDOM_ONLY);
     } else {
	  pat_max = flags & FFTW_ESTIMATE ? 0 :
	       (flags & FFTW_EXHAUSTIVE ? 3 :
		(flags & FFTW_PATIENT ? 2 : 1));
	  pat = plnr->timelimit >= 0 ? 0 : pat_max;

	  flags &= ~(FFTW_ESTIMATE | FFTW_MEASURE |
		     FFTW_PATIENT | FFTW_EXHAUSTIVE);

	  plnr->start_time = X(get_crude_time)();

	  /* plan at incrementally increasing patience until we run
	     out of time */
	  for (pln = 0, flags_used_for_planning = 0; pat <= pat_max; ++pat) {
	       plan *pln1;
	       unsigned tmpflags = flags | pats[pat];
	       pln1 = mkplan(plnr, tmpflags, prb, 0u);

	       if (!pln1) {
		    /* don't bother continuing if planner failed or timed out */
		    A(!pln || plnr->timed_out);
		    break;
	       }

	       X(plan_destroy_internal)(pln);
	       pln = pln1;
	       flags_used_for_planning = tmpflags;
	       pcost = pln->pcost;
	  }
     }

     if (pln) {
	  /* build apiplan */
	  p = (apiplan *) MALLOC(sizeof(apiplan), PLANS);
	  p->prb = prb;
	  p->sign = sign; /* cache for execute_dft */

	  /* re-create plan from wisdom, adding blessing */
	  p->pln = mkplan(plnr, flags_used_for_planning, prb, BLESSING);

	  /* record pcost from most recent measurement for use in X(cost) */
	  p->pln->pcost = pcost;

	  if (sizeof(trigreal) > sizeof(R)) {
	       /* this is probably faster, and we have enough trigreal
		  bits to maintain accuracy */
	       X(plan_awake)(p->pln, AWAKE_SQRTN_TABLE);
	  } else {
	       /* more accurate */
	       X(plan_awake)(p->pln, AWAKE_SINCOS);
	  }

	  /* we don't use pln for p->pln, above, since by re-creating the
	     plan we might use more patient wisdom from a timed-out mkplan */
	  X(plan_destroy_internal)(pln);
     } else
	  X(problem_destroy)(prb);

     /* discard all information not necessary to reconstruct the plan */
     plnr->adt->forget(plnr, FORGET_ACCURSED);

#ifdef FFTW_RANDOM_ESTIMATOR
     X(random_estimate_seed)++; /* subsequent "random" plans are distinct */
#endif

     if (after_planner_hook)
          after_planner_hook();
#ifdef AMD_APP_OPT_LAYER
     destroy_amd_app_layer(prb, &app_layer);
#endif
     return p;
}
#endif

void X(destroy_plan)(X(plan) p)
{
     if (p) {
          if (before_planner_hook)
               before_planner_hook();
     
          X(plan_awake)(p->pln, SLEEPY);
          X(plan_destroy_internal)(p->pln);
          X(problem_destroy)(p->prb);
          X(ifree)(p);

          if (after_planner_hook)
               after_planner_hook();
     }
}

int X(alignment_of)(R *p)
{
     return X(ialignment_of(p));
}
