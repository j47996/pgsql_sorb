/*-------------------------------------------------------------------------
 *
 * tuplesort.c
 *	  Generalized tuple sorting routines.
 *
 * This module handles sorting of heap tuples, index tuples, or single
 * Datums (and could easily support other kinds of sortable objects,
 * if necessary).  It works efficiently for both small and large amounts
 * of data.  Small amounts are sorted in-memory using qsort().	Large
 * amounts are sorted using temporary files and a standard external sort
 * algorithm.
 *
 * See Knuth, volume 3, for more than you want to know about the external
 * sorting algorithm.  We divide the input into sorted runs using replacement
 * selection, in the form of a priority tree implemented as a heap
 * (essentially his Algorithm 5.2.3H), then merge the runs using polyphase
 * merge, Knuth's Algorithm 5.4.2D.  The logical "tapes" used by Algorithm D
 * are implemented by logtape.c, which avoids space wastage by recycling
 * disk space as soon as each block is read from its "tape".
 *
 * We do not form the initial runs using Knuth's recommended replacement
 * selection data structure (Algorithm 5.4.1R), because it uses a fixed
 * number of records in memory at all times.  Since we are dealing with
 * tuples that may vary considerably in size, we want to be able to vary
 * the number of records kept in memory to ensure full utilization of the
 * allowed sort memory space.  So, we keep the tuples in a variable-size
 * heap, with the next record to go out at the top of the heap.  Like
 * Algorithm 5.4.1R, each record is stored with the run number that it
 * must go into, and we use (run number, key) as the ordering key for the
 * heap.  When the run number at the top of the heap changes, we know that
 * no more records of the prior run are left in the heap.
 *
 * The approximate amount of memory allowed for any one sort operation
 * is specified in kilobytes by the caller (most pass work_mem).  Initially,
 * we absorb tuples and simply store them in an unsorted array as long as
 * we haven't exceeded workMem.  If we reach the end of the input without
 * exceeding workMem, we sort the array using qsort() and subsequently return
 * tuples just by scanning the tuple array sequentially.  If we do exceed
 * workMem, we construct a heap using Algorithm H and begin to emit tuples
 * into sorted runs in temporary tapes, emitting just enough tuples at each
 * step to get back within the workMem limit.  Whenever the run number at
 * the top of the heap changes, we begin a new run with a new output tape
 * (selected per Algorithm D).	After the end of the input is reached,
 * we dump out remaining tuples in memory into a final run (or two),
 * then merge the runs using Algorithm D.
 *
 * When merging runs, we use a heap containing just the frontmost tuple from
 * each source run; we repeatedly output the smallest tuple and insert the
 * next tuple from its source tape (if any).  When the heap empties, the merge
 * is complete.  The basic merge algorithm thus needs very little memory ---
 * only M tuples for an M-way merge, and M is constrained to a small number.
 * However, we can still make good use of our full workMem allocation by
 * pre-reading additional tuples from each source tape.  Without prereading,
 * our access pattern to the temporary file would be very erratic; on average
 * we'd read one block from each of M source tapes during the same time that
 * we're writing M blocks to the output tape, so there is no sequentiality of
 * access at all, defeating the read-ahead methods used by most Unix kernels.
 * Worse, the output tape gets written into a very random sequence of blocks
 * of the temp file, ensuring that things will be even worse when it comes
 * time to read that tape.	A straightforward merge pass thus ends up doing a
 * lot of waiting for disk seeks.  We can improve matters by prereading from
 * each source tape sequentially, loading about workMem/M bytes from each tape
 * in turn.  Then we run the merge algorithm, writing but not reading until
 * one of the preloaded tuple series runs out.	Then we switch back to preread
 * mode, fill memory again, and repeat.  This approach helps to localize both
 * read and write accesses.
 *
 * When the caller requests random access to the sort result, we form
 * the final sorted run on a logical tape which is then "frozen", so
 * that we can access it randomly.	When the caller does not need random
 * access, we return from tuplesort_performsort() as soon as we are down
 * to one run per logical tape.  The final merge is then performed
 * on-the-fly as the caller repeatedly calls tuplesort_getXXX; this
 * saves one cycle of writing all the data out to disk and reading it in.
 *
 * Before Postgres 8.2, we always used a seven-tape polyphase merge, on the
 * grounds that 7 is the "sweet spot" on the tapes-to-passes curve according
 * to Knuth's figure 70 (section 5.4.2).  However, Knuth is assuming that
 * tape drives are expensive beasts, and in particular that there will always
 * be many more runs than tape drives.	In our implementation a "tape drive"
 * doesn't cost much more than a few Kb of memory buffers, so we can afford
 * to have lots of them.  In particular, if we can have as many tape drives
 * as sorted runs, we can eliminate any repeated I/O at all.  In the current
 * code we determine the number of tapes M on the basis of workMem: we want
 * workMem/M to be large enough that we read a fair amount of data each time
 * we preread from a tape, so as to maintain the locality of access described
 * above.  Nonetheless, with large workMem we can have many tapes.

 *
 * ==============
 *
 * Sorb notes
 *
 * As an alternate to the quicksort core used for the internal sort we
 * can use "Sorb".  This is an internal merge sort, with optimal merge
 * scheduling.
 * The algorithm is from Software Development International, Winter 1991.
 * Extensions for bounded and dedup support, and implementation, by
 * Jeremy Harris.
 *
 * Characteristics:
 * + The sort is progressive, stable and comparison-based.
 * + The memory overhead is O(n), in this implementation one index per datum
 *   (in SortTuple shared with tape-sort).
 * + The runtime complexity is O(n log n) for random input, and O(N) for
 *   presorted or reverse-sorted input with an optimal number of comparisons.
 *   Worst-case is O(n log n); there are no pathological cases.
 * + Runs of either direction in the input are naturally identified.
 * + Only the pointers are written during the sort operation.
 * + Handling bounded-size output is done during the sort.  We could implement
 *   replicas with/without bounding if the repeated testing of the feature
 *   is a performance issue. It does not appear to be.
 * - The memory usage for bounded-size output is larger than that for the older
 *   bounded-heap implementation, due to the in-progress merge sublists.
 *   It might cost more processing also; this is unclear.
 * + Handling of unique output is done during the sort; this is requested for
 *   the heap-sort case when called under a Unique node. The dedup is also
 *   done in the tape sort, during the writing of each tape.  Again, replica
 *   implementations do not appear to help.
 *
 * Random-access is limited in this module to rescan, mark- and restore-pos,
 * and reverse-access read.  We can handle all but the last easily; the
 * reverse-read requires a reverse-linked list.  This is built only on the first
 * reverse access (or the transition to a heap, for supporting the external
 * sort).  The additional link field is shoehorned into an already-used field
 * so the SortTuple structure is no larger, at the cost of more instructions
 * to access.
 *
 * The progressive aspect (work being done during input) means that there is
 * potential for useful overlap between work done here and readaheads done
 * for our data source.  It is not clear how significant this will be.  A
 * possible downside is the increased active instruction-space (and I-cache
 * loading).  The intermediate timing reports also become hard to compare
 * between the old and new implementations.
 *
 * Internals:
 *  Sorb runs in three phases.
 * 1) Run-linker:  Raw input is linked into a sorted ascending list for as
 *  long as a non-descending or descending run persists.  The minimum run
 *  length (apart from the very last input datum) is two, on random input
 *  the average run length is 2.7 and on presorted input the entire stream
 *  is one run (and checked as such).
 * 2) Merge-scheduler:  A short (~30) array of hooks is maintained on which
 *  can be hung sorted list.  A list produced by the linker is either
 *  hung on the first hook (if the hook is free) or merged with the list
 *  taken from the hook.  Merging is repeated working along the array of
 *  hooks until a free hook has been reached.  The array size is a constraint
 *  on the maximum data count; n hooks handling (worst-case) 2^(n+1)-1.  Thus
 *  31 hooks manages 4G data elements.  On random input the pair of lists
 *  being merged at each stage have roughly equal length; this gives an
 *  optimal number of compares for the sort.
 * 3) Collector:  On input EOF the hooks array is swept from start to end
 *  merging any lists found.  The final list is now in sorted order and
 *  can be walked from start to end to output the data.
 *
 * Possible extensions:
 *
 *  The Unique node could be collapsed when the underlying sort supports
 * dedup; the work it does is wasted.
 *
 *  The Sort node costing in the planner could be adjusted when the dedup
 * is required and supported.
 *
 *  If the input keys have a well-distributed most-significant set of bits,
 * eg text, it is possible to front-end Sorb with one stage of big-endian
 * radix sort to split into bins, running Sorb separately for each bin.
 * The final merge stage is merely to take each (now sorted) bin in order.
 * Apart from the distribution requirement, performance is best when
 * datum comparisons are expensive and datum reads are not.  Prototype
 * testing shows both compares and reads at ~70% of baseline Sorb
 * (integer data, N= 10^3 to 10^8) when using M=256 bins; worse with
 * fewer bins and/or more data.
 *  The cost is more memory; still O(log n) for this component but the
 * crossover point where the O(n) becomes larger is n ~= 2000. There is a
 * certain amount of coding complexity.
 * The index-hash case might use this - but apparently it is not used much.
 * Text data might use this.
 *
 *  We could implement output-progressivity for the final list merge,
 * only doing a comparison between the head elements of the penultimate
 * lists on demand from the data-sink.  This would be an advantage should
 * only part of the output be requested (probably unlikely) or if any
 * significant overlap would result with downstream processing.  This
 * seems unlikely in the absence of batching or an MP implementation
 * of the Postgres executor.
 *
 *  We could free memory as each tuple is read by the data-sink, if we
 * know it will not be needed again (ie. randomAccess was not requested).
 * This would ease memory-pressure but probably be less efficient than
 * the current one-shot free.  The peak memory usa would not change.
 * 
 *  There is potential for a low-order MP Sorb implementation.  The merge
 * stages are independent apart from the ordering required to maintain a
 * stable sort.  The coding complexity would be large.
 *
 *  A more dynamic division of the availible memory between the concurrently
 * running sorts would surely perform better than the current fixed values
 * of "work_mem" limit per sort.
 *
 * ==============
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/tuplesort.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>

#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/index.h"
#include "commands/tablespace.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "pg_trace.h"
#include "utils/datum.h"
#include "utils/logtape.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/pg_rusage.h"
#include "utils/rel.h"
#include "utils/sortsupport.h"
#include "utils/tuplesort.h"

#ifndef nelements
# define nelements(arr) ( sizeof(arr) / sizeof(*arr) )
#endif


/* sort-type codes for sort__start probes */
#define HEAP_SORT		0
#define INDEX_SORT		1
#define DATUM_SORT		2
#define CLUSTER_SORT	3

/* GUC variables */
#ifdef TRACE_SORT
bool		trace_sort = false;
#endif

#ifdef DEBUG_BOUNDED_SORT
bool		optimize_bounded_sort = true;
#endif
bool		optimize_dedup_sort = true;
bool		enable_intmerge_sort = true;


/*
 * The objects we actually sort are SortTuple structs.	These contain
 * a pointer to the tuple proper (might be a MinimalTuple or IndexTuple),
 * which is a separate palloc chunk --- we assume it is just one chunk and
 * can be freed by a simple pfree().  SortTuples also contain the tuple's
 * first key column in Datum/nullflag format, and an index integer.
 *
 * Storing the first key column lets us save heap_getattr or index_getattr
 * calls during tuple comparisons.	We could extract and save all the key
 * columns not just the first, but this would increase code complexity and
 * overhead, and wouldn't actually save any comparison cycles in the common
 * case where the first key determines the comparison result.  Note that
 * for a pass-by-reference datatype, datum1 points into the "tuple" storage.
 *
 * When sorting single Datums, the data value is represented directly by
 * datum1/isnull1.	If the datatype is pass-by-reference and isnull1 is false,
 * then datum1 points to a separately palloc'd data value that is also pointed
 * to by the "tuple" pointer; otherwise "tuple" is NULL.
 *
 * While building initial runs, tupindex holds the tuple's run number.  During
 * merge passes, we re-use it to hold the input tape number that each tuple in
 * the heap was read from, or to hold the index of the next tuple pre-read
 * from the same tape in the case of pre-read entries.	tupindex goes unused
 * if the sort occurs entirely in memory.
 *
 * While running the sorb variant internal sort, tupindex is reused as the index
 * in the memTuples array of the next item in a linked-list.  An index is used
 * rather than the more natural pointer bwcause the growth of memTuples may need
 * a copy; the alternative would be a specialised copy which adjusted pointers.
 * For the transition from sorb to tapes and to support reverse-access, we need
 * a reverse index to give us a doubly-linked list; this is shoehorned into the
 * upper bits of the integer holding "isnull1".
 */
#define PG_INT_BITS ((sizeof(int) * CHAR_BIT))

typedef struct SortTuple
{
	void	   *tuple;			/* the tuple proper */
	Datum		datum1;			/* value of first key column */
	int			isnull1 : 1;	/* is first key column NULL? */
	int			prev    : PG_INT_BITS-1;
	int			tupindex;		/* see notes above */
} SortTuple;

#define next tupindex			/* list used by the sorb method */

/*
 * Possible states of a Tuplesort object.  These denote the states that
 * persist between calls of Tuplesort routines.
 */
typedef enum
{
	TSS_SORB,					/* Loading tuples using alternate internal sort; */
								/*  still within memory limit                    */
	TSS_INITIAL,				/* Loading tuples; still within memory limit */
	TSS_BOUNDED,				/* Loading tuples into bounded-size heap */
	TSS_BUILDRUNS,				/* Loading tuples; writing to tape */
	TSS_SORB_ONESHOT,			/* Nonprogressive alternate internal sort */
	TSS_SORTEDINMEM,			/* Sort completed entirely in memory */
	TSS_SORTEDONTAPE,			/* Sort completed, final run is on tape */
	TSS_FINALMERGE				/* Performing final merge on-the-fly */
} TupSortStatus;

/*
 * Parameters for calculation of number of tapes to use --- see inittapes()
 * and tuplesort_merge_order().
 *
 * In this calculation we assume that each tape will cost us about 3 blocks
 * worth of buffer space (which is an underestimate for very large data
 * volumes, but it's probably close enough --- see logtape.c).
 *
 * MERGE_BUFFER_SIZE is how much data we'd like to read from each input
 * tape during a preread cycle (see discussion at top of file).
 */
#define MINORDER		6		/* minimum merge order */
#define TAPE_BUFFER_OVERHEAD		(BLCKSZ * 3)
#define MERGE_BUFFER_SIZE			(BLCKSZ * 32)

typedef int (*SortTupleComparator) (const SortTuple *a, const SortTuple *b,
												Tuplesortstate *state);

/*
 * Private state of a Tuplesort operation.
 */
struct Tuplesortstate
{
	TupSortStatus status;		/* enumerated value as shown above */
	int			nKeys;			/* number of columns in sort key */
	bool		randomAccess;	/* did caller request random access? */
	bool		bounded;		/* did caller specify a maximum number of
								 * tuples to return? */
	bool		boundUsed;		/* true if we made use of a bounded heap */
	int			bound;			/* if bounded, the maximum number of tuples */
	int64		availMem;		/* remaining memory available, in bytes */
	int64		allowedMem;		/* total memory allowed, in bytes */
	int			maxTapes;		/* number of tapes (Knuth's T) */
	int			tapeRange;		/* maxTapes-1 (Knuth's P) */
	MemoryContext sortcontext;	/* memory context holding all sort data */
	LogicalTapeSet *tapeset;	/* logtape.c object for tapes in a temp file */

	/*
	 * These function pointers decouple the routines that must know what kind
	 * of tuple we are sorting from the routines that don't need to know it.
	 * They are set up by the tuplesort_begin_xxx routines.
	 *
	 * Function to compare two tuples; result is per qsort() convention, ie:
	 * <0, 0, >0 according as a<b, a=b, a>b.  The API must match
	 * qsort_arg_comparator.
	 */
	SortTupleComparator comparetup;

	/*
	 * Function to copy a supplied input tuple into palloc'd space and set up
	 * its SortTuple representation (ie, set tuple/datum1/isnull1).  Also,
	 * state->availMem must be decreased by the amount of space used for the
	 * tuple copy (note the SortTuple struct itself is not counted).
	 */
	void		(*copytup) (Tuplesortstate *state, SortTuple *stup, void *tup);

	/*
	 * Function to write a stored tuple onto tape.	The representation of the
	 * tuple on tape need not be the same as it is in memory; requirements on
	 * the tape representation are given below.  After writing the tuple,
	 * pfree() the out-of-line data (not the SortTuple struct!), and increase
	 * state->availMem by the amount of memory space thereby released.
	 */
	void		(*writetup) (Tuplesortstate *state, int tapenum,
										 SortTuple *stup);

	/*
	 * Function to read a stored tuple from tape back into memory. 'len' is
	 * the already-read length of the stored tuple.  Create a palloc'd copy,
	 * initialize tuple/datum1/isnull1 in the target SortTuple struct, and
	 * decrease state->availMem by the amount of memory space consumed.
	 */
	void		(*readtup) (Tuplesortstate *state, SortTuple *stup,
										int tapenum, unsigned int len);

	/*
	 * Function to reverse the sort direction from its current state. (We
	 * could dispense with this if we wanted to enforce that all variants
	 * represent the sort key information alike.)
	 */
	void		(*reversedirection) (Tuplesortstate *state);

	/*
	 * This array holds the tuples now in sort memory.	If we are in state
	 * INITIAL, the tuples are in no particular order; if we are in state
	 * SORTEDINMEM, the tuples are in final sorted order; in states BUILDRUNS
	 * and FINALMERGE, the tuples are organized in "heap" order per Algorithm
	 * H.  (Note that memtupcount only counts the tuples that are part of the
	 * heap --- during merge passes, memtuples[] entries beyond tapeRange are
	 * never in the heap and are used to hold pre-read tuples.)  In state
	 * SORTEDONTAPE, the array is not used.
	 */
	SortTuple  *memtuples;		/* array of SortTuple structs */
	int			memtupcount;	/* number of tuples currently present */
	int			memtupsize;		/* allocated length of memtuples array */
	bool		growmemtuples;	/* memtuples' growth still underway? */

	/*
	 * While building initial runs, this is the current output run number
	 * (starting at 0).  Afterwards, it is the number of initial runs we made.
	 */
	int			currentRun;

	/*
	 * Unless otherwise noted, all pointer variables below are pointers to
	 * arrays of length maxTapes, holding per-tape data.
	 */

	/*
	 * These variables are only used during merge passes.  mergeactive[i] is
	 * true if we are reading an input run from (actual) tape number i and
	 * have not yet exhausted that run.  mergenext[i] is the memtuples index
	 * of the next pre-read tuple (next to be loaded into the heap) for tape
	 * i, or 0 if we are out of pre-read tuples.  mergelast[i] similarly
	 * points to the last pre-read tuple from each tape.  mergeavailslots[i]
	 * is the number of unused memtuples[] slots reserved for tape i, and
	 * mergeavailmem[i] is the amount of unused space allocated for tape i.
	 * mergefreelist and mergefirstfree keep track of unused locations in the
	 * memtuples[] array.  The memtuples[].tupindex fields link together
	 * pre-read tuples for each tape as well as recycled locations in
	 * mergefreelist. It is OK to use 0 as a null link in these lists, because
	 * memtuples[0] is part of the merge heap and is never a pre-read tuple.
	 */
	bool	   *mergeactive;	/* active input run source? */
	int		   *mergenext;		/* first preread tuple for each source */
	int		   *mergelast;		/* last preread tuple for each source */
	int		   *mergeavailslots;	/* slots left for prereading each tape */
	int64	   *mergeavailmem;	/* availMem for prereading each tape */
	int			mergefreelist;	/* head of freelist of recycled slots */
	int			mergefirstfree; /* first slot never used in this merge */

	/*
	 * Variables for Algorithm D.  Note that destTape is a "logical" tape
	 * number, ie, an index into the tp_xxx[] arrays.  Be careful to keep
	 * "logical" and "actual" tape numbers straight!
	 */
	int			Level;			/* Knuth's l */
	int			destTape;		/* current output tape (Knuth's j, less 1) */
	int		   *tp_fib;			/* Target Fibonacci run counts (A[]) */
	int		   *tp_runs;		/* # of real runs on each tape */
	int		   *tp_dummy;		/* # of dummy runs for each tape (D[]) */
	int		   *tp_tapenum;		/* Actual tape numbers (TAPE[]) */
	int			activeTapes;	/* # of active input tapes in merge pass */

	/*
	 * These variables are used after completion of sorting to keep track of
	 * the next tuple to return.  (In the tape case, the tape's current read
	 * position is also critical state.)
	 */
	int			result_tape;	/* actual tape number of finished output */
	int			current;		/* array index (only used if SORTEDINMEM) */
	bool		eof_reached;	/* reached EOF (needed for cursors) */

	/* markpos_xxx holds marked position for mark and restore */
	long		markpos_block;	/* tape block# (only used if SORTEDONTAPE) */
	int			markpos_offset; /* saved "current", or offset in tape block */
	bool		markpos_eof;	/* saved "eof_reached" */

	/*
	 * These variables are specific to the MinimalTuple case; they are set by
	 * tuplesort_begin_heap and used only by the MinimalTuple routines.
	 */
	bool		dedup;
	TupleDesc	tupDesc;
	SortSupport sortKeys;		/* array of length nKeys */

	/*
	 * This variable is shared by the single-key MinimalTuple case and the
	 * Datum case (which both use qsort_ssup()).  Otherwise it's NULL.
	 */
	SortSupport onlyKey;

	/*
	 * These variables are specific to the CLUSTER case; they are set by
	 * tuplesort_begin_cluster.  Note CLUSTER also uses tupDesc and
	 * indexScanKey.
	 */
	IndexInfo  *indexInfo;		/* info about index being used for reference */
	EState	   *estate;			/* for evaluating index expressions */

	/*
	 * These variables are specific to the IndexTuple case; they are set by
	 * tuplesort_begin_index_xxx and used only by the IndexTuple routines.
	 */
	Relation	heapRel;		/* table the index is being built on */
	Relation	indexRel;		/* index being built */

	/* These are specific to the index_btree subcase: */
	ScanKey		indexScanKey;
	bool		enforceUnique;	/* complain if we find duplicate tuples */

	/* These are specific to the index_hash subcase: */
	uint32		hash_mask;		/* mask for sortable part of hash code */

	/*
	 * These variables are specific to the Datum case; they are set by
	 * tuplesort_begin_datum and used only by the DatumTuple routines.
	 */
	Oid			datumType;
	/* we need typelen and byval in order to know how to copy the Datums. */
	int			datumTypeLen;
	bool		datumTypeByVal;

	/*
	 * These variables are specific to the sorb algorithm:
	 * "runhooks" is the list used for scheduling merges in the hope of
	 * balanced sizes (for best performance).  "maxhook" is the highest
	 * number element of runhooks.
	 * "list_end" is the current list tail for the hook-0 list while in
	 * phases 1/2, and the last list item for which a reverse-chain has
	 * been built by phase 3 (final merge).
	 * "runlen" identifies, for phase 1, the current input run direction
	 * and size, negative indicating an decreasing-order run.
	 * "list_start" is the head of the sorted list after phase 3.
	 * "reverse_linkage" is true once a complete reverse chain is built.
	 * We also use "mergefreelist".
	 */
#define NHOOKS 40
	int			runhooks[NHOOKS];
	int			maxhook;
	int			list_start;
	int			list_end;
	int			runlen;
	bool		reverse_linkage;

	/*
	 * Resource snapshot for time of sort start.
	 */
#ifdef TRACE_SORT
	PGRUsage	ru_start;
#endif
};

#define COMPARETUP(state,a,b)	((*(state)->comparetup) (a, b, state))
#define COPYTUP(state,stup,tup) ((*(state)->copytup) (state, stup, tup))
#define WRITETUP(state,tape,stup)	((*(state)->writetup) (state, tape, stup))
#define READTUP(state,stup,tape,len) ((*(state)->readtup) (state, stup, tape, len))
#define REVERSEDIRECTION(state) ((*(state)->reversedirection) (state))
#define LACKMEM(state)		((state)->availMem < 0)
#define USEMEM(state,amt)	((state)->availMem -= (amt))
#define FREEMEM(state,amt)	((state)->availMem += (amt))

/*
 * NOTES about on-tape representation of tuples:
 *
 * We require the first "unsigned int" of a stored tuple to be the total size
 * on-tape of the tuple, including itself (so it is never zero; an all-zero
 * unsigned int is used to delimit runs).  The remainder of the stored tuple
 * may or may not match the in-memory representation of the tuple ---
 * any conversion needed is the job of the writetup and readtup routines.
 *
 * If state->randomAccess is true, then the stored representation of the
 * tuple must be followed by another "unsigned int" that is a copy of the
 * length --- so the total tape space used is actually sizeof(unsigned int)
 * more than the stored length value.  This allows read-backwards.	When
 * randomAccess is not true, the write/read routines may omit the extra
 * length word.
 *
 * writetup is expected to write both length words as well as the tuple
 * data.  When readtup is called, the tape is positioned just after the
 * front length word; readtup must read the tuple data and advance past
 * the back length word (if present).
 *
 * The write/read routines can make use of the tuple description data
 * stored in the Tuplesortstate record, if needed.	They are also expected
 * to adjust state->availMem by the amount of memory space (not tape space!)
 * released or consumed.  There is no error return from either writetup
 * or readtup; they should ereport() on failure.
 *
 *
 * NOTES about memory consumption calculations:
 *
 * We count space allocated for tuples against the workMem limit, plus
 * the space used by the variable-size memtuples array.  Fixed-size space
 * is not counted; it's small enough to not be interesting.
 *
 * Note that we count actual space used (as shown by GetMemoryChunkSpace)
 * rather than the originally-requested size.  This is important since
 * palloc can add substantial overhead.  It's not a complete answer since
 * we won't count any wasted space in palloc allocation blocks, but it's
 * a lot better than what we were doing before 7.3.
 */

/* When using this macro, beware of double evaluation of len */
#define LogicalTapeReadExact(tapeset, tapenum, ptr, len) \
	do { \
		if (LogicalTapeRead(tapeset, tapenum, ptr, len) != (size_t) (len)) \
			elog(ERROR, "unexpected end of data"); \
	} while(0)


static Tuplesortstate *tuplesort_begin_common(int workMem, bool randomAccess);
static void puttuple_common(Tuplesortstate *state, SortTuple *tuple);
static void inittapes(Tuplesortstate *state);
static void selectnewtape(Tuplesortstate *state);
static void mergeruns(Tuplesortstate *state);
static void mergeonerun(Tuplesortstate *state);
static void beginmerge(Tuplesortstate *state);
static void mergepreread(Tuplesortstate *state);
static void mergeprereadone(Tuplesortstate *state, int srcTape);
static void dumptuples(Tuplesortstate *state, bool alltuples);
static void make_bounded_heap(Tuplesortstate *state);
static void sort_bounded_heap(Tuplesortstate *state);
static void tuplesort_heap_insert(Tuplesortstate *state, SortTuple *tuple,
					  int tupleindex, bool checkIndex);
static void tuplesort_heap_siftup(Tuplesortstate *state, bool checkIndex);
static unsigned int getlen(Tuplesortstate *state, int tapenum, bool eofOK);
static void markrunend(Tuplesortstate *state, int tapenum);
static int comparetup_heap(const SortTuple *a, const SortTuple *b,
				Tuplesortstate *state);
static void copytup_heap(Tuplesortstate *state, SortTuple *stup, void *tup);
static void writetup_heap(Tuplesortstate *state, int tapenum,
			  SortTuple *stup);
static void readtup_heap(Tuplesortstate *state, SortTuple *stup,
			 int tapenum, unsigned int len);
static void reversedirection_heap(Tuplesortstate *state);
static int comparetup_cluster(const SortTuple *a, const SortTuple *b,
				   Tuplesortstate *state);
static void copytup_cluster(Tuplesortstate *state, SortTuple *stup, void *tup);
static void writetup_cluster(Tuplesortstate *state, int tapenum,
				 SortTuple *stup);
static void readtup_cluster(Tuplesortstate *state, SortTuple *stup,
				int tapenum, unsigned int len);
static int comparetup_index_btree(const SortTuple *a, const SortTuple *b,
					   Tuplesortstate *state);
static int comparetup_index_hash(const SortTuple *a, const SortTuple *b,
					  Tuplesortstate *state);
static void copytup_index(Tuplesortstate *state, SortTuple *stup, void *tup);
static void writetup_index(Tuplesortstate *state, int tapenum,
			   SortTuple *stup);
static void readtup_index(Tuplesortstate *state, SortTuple *stup,
			  int tapenum, unsigned int len);
static void reversedirection_index_btree(Tuplesortstate *state);
static void reversedirection_index_hash(Tuplesortstate *state);
static int comparetup_datum(const SortTuple *a, const SortTuple *b,
				 Tuplesortstate *state);
static void copytup_datum(Tuplesortstate *state, SortTuple *stup, void *tup);
static void writetup_datum(Tuplesortstate *state, int tapenum,
			   SortTuple *stup);
static void readtup_datum(Tuplesortstate *state, SortTuple *stup,
			  int tapenum, unsigned int len);
static void reversedirection_datum(Tuplesortstate *state);
static void free_sort_tuple(Tuplesortstate *state, SortTuple *stup);

/*
 * Special versions of qsort just for SortTuple objects.  qsort_tuple() sorts
 * any variant of SortTuples, using the appropriate comparetup function.
 * qsort_ssup() is specialized for the case where the comparetup function
 * reduces to ApplySortComparator(), that is single-key MinimalTuple sorts
 * and Datum sorts.
 */
#include "qsort_tuple.c"


/*
 *		tuplesort_begin_xxx
 *
 * Initialize for a tuple sort operation.
 *
 * After calling tuplesort_begin, the caller should call tuplesort_putXXX
 * zero or more times, then call tuplesort_performsort when all the tuples
 * have been supplied.	After performsort, retrieve the tuples in sorted
 * order by calling tuplesort_getXXX until it returns false/NULL.  (If random
 * access was requested, rescan, markpos, and restorepos can also be called.)
 * Call tuplesort_end to terminate the operation and release memory/disk space.
 *
 * Each variant of tuplesort_begin has a workMem parameter specifying the
 * maximum number of kilobytes of RAM to use before spilling data to disk.
 * (The normal value of this parameter is work_mem, but some callers use
 * other values.)  Each variant also has a randomAccess parameter specifying
 * whether the caller needs non-sequential access to the sort result.
 */

static Tuplesortstate *
tuplesort_begin_common(int workMem, bool randomAccess)
{
	Tuplesortstate *state;
	MemoryContext sortcontext;
	MemoryContext oldcontext;

	/*
	 * Create a working memory context for this sort operation. All data
	 * needed by the sort will live inside this context.
	 */
	sortcontext = AllocSetContextCreate(CurrentMemoryContext,
										"TupleSort",
										ALLOCSET_DEFAULT_MINSIZE,
										ALLOCSET_DEFAULT_INITSIZE,
										ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * Make the Tuplesortstate within the per-sort context.  This way, we
	 * don't need a separate pfree() operation for it at shutdown.
	 */
	oldcontext = MemoryContextSwitchTo(sortcontext);

	state = (Tuplesortstate *) palloc0(sizeof(Tuplesortstate));

#ifdef TRACE_SORT
	if (trace_sort)
		pg_rusage_init(&state->ru_start);
#endif

	if (enable_intmerge_sort)
	{ /* Go to TSS_SORB if dedup or bounded; to TSS_SORB_ONESHOT if input ends early */
		int * hp;
		for(hp = state->runhooks; hp < state->runhooks + NHOOKS; hp++)
			*hp = -1;
		state->list_start = -1;
		state->maxhook= 0;
		state->mergefreelist= -1;
	}
	state->status = TSS_INITIAL;

	state->randomAccess = randomAccess;
	state->dedup = false;
	state->bounded = false;
	state->boundUsed = false;
	state->allowedMem = workMem * (int64) 1024;
	state->availMem = state->allowedMem;
	state->sortcontext = sortcontext;
	state->tapeset = NULL;

	state->memtupcount = 0;
	state->memtupsize = 1024;	/* initial guess */
	state->growmemtuples = true;
	state->memtuples = (SortTuple *) palloc(state->memtupsize * sizeof(SortTuple));

	USEMEM(state, GetMemoryChunkSpace(state->memtuples));

	/* workMem must be large enough for the minimal memtuples array */
	if (LACKMEM(state))
		elog(ERROR, "insufficient memory allowed for sort");

	state->currentRun = 0;

	/*
	 * maxTapes, tapeRange, and Algorithm D variables will be initialized by
	 * inittapes(), if needed
	 */

	state->result_tape = -1;	/* flag that result tape has not been formed */

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_heap(TupleDesc tupDesc,
					 int nkeys, AttrNumber *attNums,
					 Oid *sortOperators, Oid *sortCollations,
					 bool *nullsFirstFlags,
					 int workMem, bool dedup, bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, randomAccess);
	MemoryContext oldcontext;
	int			i;

	oldcontext = MemoryContextSwitchTo(state->sortcontext);

	AssertArg(nkeys > 0);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin heap sort: nkeys = %d, workMem = %d, dedup = %c, randomAccess = %c",
			 nkeys, workMem,
			 dedup ? 't' : 'f', randomAccess ? 't' : 'f');
#endif

	state->nKeys = nkeys;
	if ((state->dedup = dedup && optimize_dedup_sort) && enable_intmerge_sort)
		state->status = TSS_SORB;

	TRACE_POSTGRESQL_SORT_START(HEAP_SORT,
								false,	/* no unique check */
								nkeys,
								workMem,
								randomAccess);

	state->comparetup = comparetup_heap;
	state->copytup = copytup_heap;
	state->writetup = writetup_heap;
	state->readtup = readtup_heap;
	state->reversedirection = reversedirection_heap;

	state->tupDesc = tupDesc;	/* assume we need not copy tupDesc */

	/* Prepare SortSupport data for each column */
	state->sortKeys = (SortSupport) palloc0(nkeys * sizeof(SortSupportData));

	for (i = 0; i < nkeys; i++)
	{
		SortSupport sortKey = state->sortKeys + i;

		AssertArg(attNums[i] != 0);
		AssertArg(sortOperators[i] != 0);

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = sortCollations[i];
		sortKey->ssup_nulls_first = nullsFirstFlags[i];
		sortKey->ssup_attno = attNums[i];

		PrepareSortSupportFromOrderingOp(sortOperators[i], sortKey);
	}

	if (nkeys == 1)
		state->onlyKey = state->sortKeys;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_cluster(TupleDesc tupDesc,
						Relation indexRel,
						int workMem, bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, randomAccess);
	MemoryContext oldcontext;

	Assert(indexRel->rd_rel->relam == BTREE_AM_OID);

	oldcontext = MemoryContextSwitchTo(state->sortcontext);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin cluster sort: nkeys = %d, workMem = %d, randomAccess = %c",
			 RelationGetNumberOfAttributes(indexRel),
			 workMem, randomAccess ? 't' : 'f');
#endif

	state->nKeys = RelationGetNumberOfAttributes(indexRel);

	TRACE_POSTGRESQL_SORT_START(CLUSTER_SORT,
								false,	/* no unique check */
								state->nKeys,
								workMem,
								randomAccess);

	state->comparetup = comparetup_cluster;
	state->copytup = copytup_cluster;
	state->writetup = writetup_cluster;
	state->readtup = readtup_cluster;
	state->reversedirection = reversedirection_index_btree;

	state->indexInfo = BuildIndexInfo(indexRel);
	state->indexScanKey = _bt_mkscankey_nodata(indexRel);

	state->tupDesc = tupDesc;	/* assume we need not copy tupDesc */

	if (state->indexInfo->ii_Expressions != NULL)
	{
		TupleTableSlot *slot;
		ExprContext *econtext;

		/*
		 * We will need to use FormIndexDatum to evaluate the index
		 * expressions.  To do that, we need an EState, as well as a
		 * TupleTableSlot to put the table tuples into.  The econtext's
		 * scantuple has to point to that slot, too.
		 */
		state->estate = CreateExecutorState();
		slot = MakeSingleTupleTableSlot(tupDesc);
		econtext = GetPerTupleExprContext(state->estate);
		econtext->ecxt_scantuple = slot;
	}

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_index_btree(Relation heapRel,
							Relation indexRel,
							bool enforceUnique,
							int workMem, bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, randomAccess);
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(state->sortcontext);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin index_bt sort: unique = %c, workMem = %d, randomAccess = %c",
			 enforceUnique ? 't' : 'f',
			 workMem, randomAccess ? 't' : 'f');
#endif

	state->nKeys = RelationGetNumberOfAttributes(indexRel);

	TRACE_POSTGRESQL_SORT_START(INDEX_SORT,
								enforceUnique,
								state->nKeys,
								workMem,
								randomAccess);

	state->comparetup = comparetup_index_btree;
	state->copytup = copytup_index;
	state->writetup = writetup_index;
	state->readtup = readtup_index;
	state->reversedirection = reversedirection_index_btree;

	state->heapRel = heapRel;
	state->indexRel = indexRel;
	state->indexScanKey = _bt_mkscankey_nodata(indexRel);
	state->enforceUnique = enforceUnique;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_index_hash(Relation heapRel,
						   Relation indexRel,
						   uint32 hash_mask,
						   int workMem, bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, randomAccess);
	MemoryContext oldcontext;

	oldcontext = MemoryContextSwitchTo(state->sortcontext);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
		"begin index_h sort: hash_mask = 0x%x, workMem = %d, randomAccess = %c",
			 hash_mask,
			 workMem, randomAccess ? 't' : 'f');
#endif

	state->nKeys = 1;			/* Only one sort column, the hash code */

	state->comparetup = comparetup_index_hash;
	state->copytup = copytup_index;
	state->writetup = writetup_index;
	state->readtup = readtup_index;
	state->reversedirection = reversedirection_index_hash;

	state->heapRel = heapRel;
	state->indexRel = indexRel;

	state->hash_mask = hash_mask;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_datum(Oid datumType, Oid sortOperator, Oid sortCollation,
					  bool nullsFirstFlag,
					  int workMem, bool randomAccess)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, randomAccess);
	MemoryContext oldcontext;
	int16		typlen;
	bool		typbyval;

	oldcontext = MemoryContextSwitchTo(state->sortcontext);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin datum sort: workMem = %d, randomAccess = %c",
			 workMem, randomAccess ? 't' : 'f');
#endif

	state->nKeys = 1;			/* always a one-column sort */

	TRACE_POSTGRESQL_SORT_START(DATUM_SORT,
								false,	/* no unique check */
								1,
								workMem,
								randomAccess);

	state->comparetup = comparetup_datum;
	state->copytup = copytup_datum;
	state->writetup = writetup_datum;
	state->readtup = readtup_datum;
	state->reversedirection = reversedirection_datum;

	state->datumType = datumType;

	/* Prepare SortSupport data */
	state->onlyKey = (SortSupport) palloc0(sizeof(SortSupportData));

	state->onlyKey->ssup_cxt = CurrentMemoryContext;
	state->onlyKey->ssup_collation = sortCollation;
	state->onlyKey->ssup_nulls_first = nullsFirstFlag;

	PrepareSortSupportFromOrderingOp(sortOperator, state->onlyKey);

	/* lookup necessary attributes of the datum type */
	get_typlenbyval(datumType, &typlen, &typbyval);
	state->datumTypeLen = typlen;
	state->datumTypeByVal = typbyval;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

/*
 * tuplesort_set_bound
 *
 *	Advise tuplesort that at most the first N result tuples are required.
 *
 * Must be called before inserting any tuples.	(Actually, we could allow it
 * as long as the sort hasn't spilled to disk, but there seems no need for
 * delayed calls at the moment.)
 *
 * This is a hint only. The tuplesort may still return more tuples than
 * requested.
 */
void
tuplesort_set_bound(Tuplesortstate *state, int64 bound)
{
	/* Assert we're called before loading any tuples */
	Assert(state->status == TSS_INITIAL || state->status == TSS_SORB);
	Assert(state->memtupcount == 0);
	Assert(!state->bounded);

#ifdef DEBUG_BOUNDED_SORT
	/* Honor GUC setting that disables the feature (for easy testing) */
	if (!optimize_bounded_sort)
		return;
#endif

	/* We want to be able to compute bound * 2, so limit the setting */
	if (bound > (int64) (INT_MAX / 2))
		return;

	state->bounded = true;
	state->bound = (int) bound;
	if( state->status == TSS_INITIAL && enable_intmerge_sort)
	{
		state->status = TSS_SORB;
		state->boundUsed = true;
		elog(LOG,"%s: set boundUsed", __FUNCTION__);
	}
}

/*
 * tuplesort_end
 *
 *	Release resources and clean up.
 *
 * NOTE: after calling this, any pointers returned by tuplesort_getXXX are
 * pointing to garbage.  Be careful not to attempt to use or free such
 * pointers afterwards!
 */
void
tuplesort_end(Tuplesortstate *state)
{
	/* context swap probably not needed, but let's be safe */
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

#ifdef TRACE_SORT
	long		spaceUsed;

	if (state->tapeset)
		spaceUsed = LogicalTapeSetBlocks(state->tapeset);
	else
		spaceUsed = (state->allowedMem - state->availMem + 1023) / 1024;
#endif

	/*
	 * Delete temporary "tape" files, if any.
	 *
	 * Note: want to include this in reported total cost of sort, hence need
	 * for two #ifdef TRACE_SORT sections.
	 */
	if (state->tapeset)
		LogicalTapeSetClose(state->tapeset);

#ifdef TRACE_SORT
	if (trace_sort)
	{
		if (state->tapeset)
			elog(NOTICE, "external sort ended, %ld disk blocks used: %s",
				 spaceUsed, pg_rusage_show(&state->ru_start));
		else
			elog(NOTICE, "internal sort ended, %ld KB used: %s",
				 spaceUsed, pg_rusage_show(&state->ru_start));
	}

	TRACE_POSTGRESQL_SORT_DONE(state->tapeset != NULL, spaceUsed);
#else

	/*
	 * If you disabled TRACE_SORT, you can still probe sort__done, but you
	 * ain't getting space-used stats.
	 */
	TRACE_POSTGRESQL_SORT_DONE(state->tapeset != NULL, 0L);
#endif

	/* Free any execution state created for CLUSTER case */
	if (state->estate != NULL)
	{
		ExprContext *econtext = GetPerTupleExprContext(state->estate);

		ExecDropSingleTupleTableSlot(econtext->ecxt_scantuple);
		FreeExecutorState(state->estate);
	}

	MemoryContextSwitchTo(oldcontext);

	/*
	 * Free the per-sort memory context, thereby releasing all working memory,
	 * including the Tuplesortstate struct itself.
	 */
	MemoryContextDelete(state->sortcontext);
}

/*
 * Grow the memtuples[] array, if possible within our memory constraint.  We
 * must not exceed INT_MAX tuples in memory or the caller-provided memory
 * limit.  Return TRUE if we were able to enlarge the array, FALSE if not.
 *
 * Normally, at each increment we double the size of the array.  When doing
 * that would exceed a limit, we attempt one last, smaller increase (and then
 * clear the growmemtuples flag so we don't try any more).  That allows us to
 * use memory as fully as permitted; sticking to the pure doubling rule could
 * result in almost half going unused.  Because availMem moves around with
 * tuple addition/removal, we need some rule to prevent making repeated small
 * increases in memtupsize, which would just be useless thrashing.  The
 * growmemtuples flag accomplishes that and also prevents useless
 * recalculations in this function.
 */
static bool
grow_memtuples(Tuplesortstate *state)
{
	int			newmemtupsize;
	int			memtupsize = state->memtupsize;
	int64		memNowUsed = state->allowedMem - state->availMem;

	/* Forget it if we've already maxed out memtuples, per comment above */
	if (!state->growmemtuples)
		return false;

	/* Select new value of memtupsize */
	if (memNowUsed <= state->availMem)
	{
		/*
		 * We've used no more than half of allowedMem; double our usage,
		 * clamping at INT_MAX tuples.
		 */
		if (memtupsize < INT_MAX / 2)
			newmemtupsize = memtupsize * 2;
		else
		{
			newmemtupsize = INT_MAX;
			state->growmemtuples = false;
		}
	}
	else
	{
		/*
		 * This will be the last increment of memtupsize.  Abandon doubling
		 * strategy and instead increase as much as we safely can.
		 *
		 * To stay within allowedMem, we can't increase memtupsize by more
		 * than availMem / sizeof(SortTuple) elements.	In practice, we want
		 * to increase it by considerably less, because we need to leave some
		 * space for the tuples to which the new array slots will refer.  We
		 * assume the new tuples will be about the same size as the tuples
		 * we've already seen, and thus we can extrapolate from the space
		 * consumption so far to estimate an appropriate new size for the
		 * memtuples array.  The optimal value might be higher or lower than
		 * this estimate, but it's hard to know that in advance.  We again
		 * clamp at INT_MAX tuples.
		 *
		 * This calculation is safe against enlarging the array so much that
		 * LACKMEM becomes true, because the memory currently used includes
		 * the present array; thus, there would be enough allowedMem for the
		 * new array elements even if no other memory were currently used.
		 *
		 * We do the arithmetic in float8, because otherwise the product of
		 * memtupsize and allowedMem could overflow.  Any inaccuracy in the
		 * result should be insignificant; but even if we computed a
		 * completely insane result, the checks below will prevent anything
		 * really bad from happening.
		 */
		double		grow_ratio;

		grow_ratio = (double) state->allowedMem / (double) memNowUsed;
		if (memtupsize * grow_ratio < INT_MAX)
			newmemtupsize = (int) (memtupsize * grow_ratio);
		else
			newmemtupsize = INT_MAX;

		/* We won't make any further enlargement attempts */
		state->growmemtuples = false;
	}

	/* Must enlarge array by at least one element, else report failure */
	if (newmemtupsize <= memtupsize)
		goto noalloc;

	/*
	 * On a 32-bit machine, allowedMem could exceed MaxAllocHugeSize.  Clamp
	 * to ensure our request won't be rejected.  Note that we can easily
	 * exhaust address space before facing this outcome.  (This is presently
	 * impossible due to guc.c's MAX_KILOBYTES limitation on work_mem, but
	 * don't rely on that at this distance.)
	 */
	if ((Size) newmemtupsize >= MaxAllocHugeSize / sizeof(SortTuple))
	{
		newmemtupsize = (int) (MaxAllocHugeSize / sizeof(SortTuple));
		state->growmemtuples = false;	/* can't grow any more */
	}

	/*
	 * We need to be sure that we do not cause LACKMEM to become true, else
	 * the space management algorithm will go nuts.  The code above should
	 * never generate a dangerous request, but to be safe, check explicitly
	 * that the array growth fits within availMem.	(We could still cause
	 * LACKMEM if the memory chunk overhead associated with the memtuples
	 * array were to increase.	That shouldn't happen with any sane value of
	 * allowedMem, because at any array size large enough to risk LACKMEM,
	 * palloc would be treating both old and new arrays as separate chunks.
	 * But we'll check LACKMEM explicitly below just in case.)
	 */
	if (state->availMem < (int64) ((newmemtupsize - memtupsize) * sizeof(SortTuple)))
		goto noalloc;

	/* OK, do it */
	FREEMEM(state, GetMemoryChunkSpace(state->memtuples));
	state->memtupsize = newmemtupsize;
	state->memtuples = (SortTuple *)
		repalloc_huge(state->memtuples,
					  state->memtupsize * sizeof(SortTuple));
	USEMEM(state, GetMemoryChunkSpace(state->memtuples));
	if (LACKMEM(state))
		elog(ERROR, "unexpected out-of-memory situation during sort");
	return true;

noalloc:
	/* If for any reason we didn't realloc, shut off future attempts */
	state->growmemtuples = false;
	return false;
}

/*
 * Accept one tuple while collecting input data for sort.
 *
 * Note that the input data is always copied; the caller need not save it.
 */
void
tuplesort_puttupleslot(Tuplesortstate *state, TupleTableSlot *slot)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;

	/*
	 * Copy the given tuple into memory we control, and decrease availMem.
	 * Then call the common code.
	 */
	COPYTUP(state, &stup, (void *) slot);

	puttuple_common(state, &stup);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Accept one tuple while collecting input data for sort.
 *
 * Note that the input data is always copied; the caller need not save it.
 */
void
tuplesort_putheaptuple(Tuplesortstate *state, HeapTuple tup)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;

	/*
	 * Copy the given tuple into memory we control, and decrease availMem.
	 * Then call the common code.
	 */
	COPYTUP(state, &stup, (void *) tup);

	puttuple_common(state, &stup);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Accept one index tuple while collecting input data for sort.
 *
 * Note that the input tuple is always copied; the caller need not save it.
 */
void
tuplesort_putindextuple(Tuplesortstate *state, IndexTuple tuple)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;

	/*
	 * Copy the given tuple into memory we control, and decrease availMem.
	 * Then call the common code.
	 */
	COPYTUP(state, &stup, (void *) tuple);

	puttuple_common(state, &stup);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Accept one Datum while collecting input data for sort.
 *
 * If the Datum is pass-by-ref type, the value will be copied.
 */
void
tuplesort_putdatum(Tuplesortstate *state, Datum val, bool isNull)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;

	/*
	 * If it's a pass-by-reference value, copy it into memory we control, and
	 * decrease availMem.  Then call the common code.
	 */
	if (isNull || state->datumTypeByVal)
	{
		stup.datum1 = val;
		stup.isnull1 = isNull;
		stup.tuple = NULL;		/* no separate storage */
	}
	else
	{
		stup.datum1 = datumCopy(val, false, state->datumTypeLen);
		stup.isnull1 = false;
		stup.tuple = DatumGetPointer(stup.datum1);
		USEMEM(state, GetMemoryChunkSpace(stup.tuple));
	}

	puttuple_common(state, &stup);

	MemoryContextSwitchTo(oldcontext);
}

/* Sorb 2nd/3rd stage: merge a pair of lists. */
/* If requested, fill in backlinks as we go, and note how far backlinking got.*/
static int
sorb_merge_basic(struct Tuplesortstate * state, int old, int new)
{
	SortTuple *memtuples = state->memtuples;
	SortTuple *op = &memtuples[old];
	SortTuple *np = &memtuples[new];
	int start;
	int end = -1;
	int cmp;
	const SortSupport onlyKey = state->onlyKey;

	CHECK_FOR_INTERRUPTS();
	cmp = cmp_ssup(op, np, onlyKey);
	if( cmp > 0 )
	{
		start = new;
		goto new_lower;
	}
	start = old;

old_lower:
	do {
		end = old;
		old = op->next;
		if( old == -1 )	/* Just link "new" list on the end */
		{
			/*
			 * Now here's an interesting question.  It seems obvious 
			 * to just link the tail of the unexhausted list after the
			 * merged list, to gain a max-length resulting merge with
			 * no effort.  But do we actually lose out in the long
			 * run?
			 * The posit of the Sorb developer was that best performance
			 * was obtained by merging lists of equal length - hence the
			 * development of the exponential merge schedule.  Does this
			 * imply that *not* doing compare work on the tail is a
			 * is a performance loss?  I do not see how to analyse this.
			 *
			 * We could implement some scheme for returning the tail
			 * separately from the merge-result, and optimally place it
			 * on a hook.  But what if it's longer than the merge? Is
			 * that an issue?  Mind, it should not happen given the
			 * (stability-losing) optimal hook placement of initially
			 * linked runs. More likely to happen is that the merge is
			 * not long enough to be optimally placed on the next hook.
			 * Also, this scheme will probably destroy stability itself.
			 *
			 * Posibly the way to view the issue is that the merged
			 * part has double the information density but the tail
			 * density is unaltered.  On that basis the tail should be
			 * re-placed on the current hook (and the merge on the next)..
			 */

			memtuples[end].next = new;
			return start;
		}
		op = &memtuples[old];

		CHECK_FOR_INTERRUPTS();
		cmp = cmp_ssup(op, np, onlyKey);
	} while( cmp <= 0 );

	memtuples[end].next = new;

new_lower:
	do {
		end = new;
		new = np->next;
		if( new == -1 )	/* Just link the "old" list on the end */
		{
			memtuples[end].next = old;
			return start;
		}
		np = &memtuples[new];

		CHECK_FOR_INTERRUPTS();
		cmp = cmp_ssup(op, np, onlyKey);
	} while( cmp > 0 );

	memtuples[end].next = old;
	goto old_lower;
}
static int
sorb_merge_ssup(struct Tuplesortstate * state, int old, int new,
			const bool backlink)
{
	SortTuple *memtuples = state->memtuples;
	SortTuple *op = &memtuples[old];
	SortTuple *np = &memtuples[new];
	int start;
	int end = -1;
	unsigned bound = state->bounded ? (unsigned)state->bound : UINT_MAX;
	unsigned cnt = 1;
	int cmp;
	SortSupport onlyKey = state->onlyKey;

	CHECK_FOR_INTERRUPTS();
	cmp = cmp_ssup(op, np, onlyKey);
	if( state->dedup  &&  cmp == 0 )
	{				/* A dup; drop one */
		int i = new;
		new = np->next;
		free_sort_tuple(state, np);
		np->next = state->mergefreelist;
		state->mergefreelist = i;
		if( new < 0 )
			return old;
		np= &memtuples[new];
		CHECK_FOR_INTERRUPTS();
		cmp = cmp_ssup(op, np, onlyKey);
	}

	if( cmp > 0 )
	{
		start = new;
		goto new_lower;
	}
	start = old;

old_lower:
	do {
		if( backlink )
			op->prev = end;
		end = old;
		old = op->next;
		if( cnt++ >= bound )
			goto bound_reached;


		if( old == -1 )	/* Just link "new" list on the end */
		{
			/*
			 * Now here's an interesting question.  It seems obvious 
			 * to just link the tail of the unexhausted list after the
			 * merged list, to gain a max-length resulting merge with
			 * no effort.  But do we actually lose out in the long
			 * run?
			 * The posit of the Sorb developer was that best performance
			 * was obtained by merging lists of equal length - hence the
			 * development of the exponential merge schedule.  Does this
			 * imply that *not* doing compare work on the tail is a
			 * is a performance loss?  I do not see how to analyse this.
			 *
			 * We could implement some scheme for returning the tail
			 * separately from the merge-result, and optimally place it
			 * on a hook.  But what if it's longer than the merge? Is
			 * that an issue?  Mind, it should not happen given the
			 * (stability-losing) optimal hook placement of initially
			 * linked runs. More likely to happen is that the merge is
			 * not long enough to be optimally placed on the next hook.
			 * Also, this scheme will probably destroy stability itself.
			 *
			 * Posibly the way to view the issue is that the merged
			 * part has double the information density but the tail
			 * density is unaltered.  On that basis the tail should be
			 * re-placed on the current hook (and the merge on the next)..
			 */

			/*
			 * If bounded but not yet there, we could walk the "new"
			 * list counting, and trim once it went over the bound -
			 * but we're not doing any more comparisons so probably
			 * not worthwhile here (unless memory is under pressure?)
			 */
			memtuples[end].next = new;
			if( backlink )
			{
				np->prev = end;
				state->list_end = new;	/* backlinking done to here */
			}
			return start;
		}
		op = &memtuples[old];

		CHECK_FOR_INTERRUPTS();
		cmp = cmp_ssup(op, np, onlyKey);
		if( state->dedup  &&  cmp == 0 )
		{
			int i = new;
			new = np->next;
			free_sort_tuple(state, &memtuples[i]);
			np->next = state->mergefreelist;
			state->mergefreelist = i;
			if( new < 0 )
				return start;
			np = &memtuples[new];
			CHECK_FOR_INTERRUPTS();
			cmp = cmp_ssup(op, np, onlyKey);
		}
	} while( cmp <= 0 );

	memtuples[end].next = new;

new_lower:
	do {
		if( backlink )
			np->prev = end;
		end = new;
		new = np->next;
		if( cnt++ >= bound )
			goto bound_reached;
		if( new == -1 )	/* Just link the "old" list on the end */
		{
			memtuples[end].next = old;
			if( backlink )
			{
				op->prev = end;
				state->list_end = old;	/* backlinking done to here */
			}
			return start;
		}
		np = &memtuples[new];

		CHECK_FOR_INTERRUPTS();
		cmp = cmp_ssup(op, np, onlyKey);
		if( state->dedup  &&  cmp == 0 )
		{
			int i = old;
			old = op->next;
			free_sort_tuple(state, &memtuples[i]);
			op->next = state->mergefreelist;
			state->mergefreelist = i;
			if( old < 0 )
				return start;
			op = &memtuples[old];
			CHECK_FOR_INTERRUPTS();
			cmp = cmp_ssup(op, np, onlyKey);
		}
	} while( cmp > 0 );

	memtuples[end].next = old;
	goto old_lower;

bound_reached:
	memtuples[end].next = -1;
	if( backlink )
		state->list_end = end;	/* backlinking done to here */

	/* We can free up the tuple memory, but not the SortTuple structs. */
	/* Put the structs on a freelist for re-use by new tuples.         */
	if( old >= 0 )
	{
		int i;
		for(i= old;; i= end)
		{
			CHECK_FOR_INTERRUPTS();
			free_sort_tuple(state, op = &memtuples[i]);
			if( (end= op->next) < 0 ) break;
		}
		memtuples[i].next= state->mergefreelist;
		state->mergefreelist= old;
	}
	if( new >= 0 )
	{
		int i;
		for(i= new;; i= end)
		{
			CHECK_FOR_INTERRUPTS();
			free_sort_tuple(state, np = &memtuples[i]);
			if( (end= np->next) < 0 ) break;
		}
		memtuples[i].next= state->mergefreelist;
		state->mergefreelist= new;
	}
	return start;
}
static int
sorb_merge(struct Tuplesortstate * state, int old, int new, const bool backlink)
{
	SortTuple *memtuples = state->memtuples;
	int start;
	int end = -1;
	unsigned bound = state->bounded ? (unsigned)state->bound : UINT_MAX;
	unsigned cnt = 1;
	int cmp;

	CHECK_FOR_INTERRUPTS();
	cmp = COMPARETUP(state, &memtuples[old], &memtuples[new]);
	if( state->dedup  &&  cmp == 0 )
	{				/* A dup; drop one */
		int i = new;
		new = memtuples[new].next;
		free_sort_tuple(state, &memtuples[i]);
		memtuples[i].next = state->mergefreelist;
		state->mergefreelist = i;
		if( new < 0 )
			return old;
		CHECK_FOR_INTERRUPTS();
		cmp = COMPARETUP(state, &memtuples[old], &memtuples[new]);
	}

	if( cmp > 0 )
	{
		start = new;
		goto new_lower;
	}
	start = old;

old_lower:
	do {
		if( backlink )
			memtuples[old].prev = end;
		end = old;
		old = memtuples[old].next;
		if( cnt++ >= bound )
			goto bound_reached;
		if( old == -1 )	/* Just link "new" list on the end */
		{
			/*
			 * If bounded but not yet there, we could walk the "new"
			 * list counting, and trim once it went over the bound -
			 * but we're not doing any more comparisons so probably
			 * not worthwhile here (unless memory is under pressure?)
			 */
			memtuples[end].next = new;
			if( backlink )
			{
				memtuples[new].prev = end;
				state->list_end = new;	/* backlinking done to here */
			}
			return start;
		}

		CHECK_FOR_INTERRUPTS();
		cmp = COMPARETUP(state, &memtuples[old], &memtuples[new]);
		if( state->dedup  &&  cmp == 0 )
		{
			int i = new;
			new = memtuples[new].next;
			free_sort_tuple(state, &memtuples[i]);
			memtuples[i].next = state->mergefreelist;
			state->mergefreelist = i;
			if( new < 0 )
				return start;
			CHECK_FOR_INTERRUPTS();
			cmp = COMPARETUP(state, &memtuples[old], &memtuples[new]);
		}
	} while( cmp <= 0 );

	memtuples[end].next = new;

new_lower:
	do {
		if( backlink )
			memtuples[new].prev = end;
		end = new;
		new = memtuples[new].next;
		if( cnt++ >= bound )
			goto bound_reached;
		if( new == -1 )	/* Just link the "old" list on the end */
		{
			memtuples[end].next = old;
			if( backlink )
			{
				memtuples[old].prev = end;
				state->list_end = old;	/* backlinking done to here */
			}
			return start;
		}

		CHECK_FOR_INTERRUPTS();
		cmp = COMPARETUP(state, &memtuples[old], &memtuples[new]);
		if( state->dedup  &&  cmp == 0 )
		{
			int i = old;
			old = memtuples[old].next;
			free_sort_tuple(state, &memtuples[i]);
			memtuples[i].next = state->mergefreelist;
			state->mergefreelist = i;
			if( old < 0 )
				return start;
			CHECK_FOR_INTERRUPTS();
			cmp = COMPARETUP(state, &memtuples[old], &memtuples[new]);
		}
	} while( cmp > 0 );

	memtuples[end].next = old;
	goto old_lower;

bound_reached:
	memtuples[end].next = -1;
	if( backlink )
		state->list_end = end;	/* backlinking done to here */

	/* We can free up the tuple memory, but not the SortTuple structs. */
	/* Put the structs on a freelist for re-use by new tuples.         */
	if( old >= 0 )
	{
		int i;
		for(i= old;; i= end)
		{
			CHECK_FOR_INTERRUPTS();
			free_sort_tuple(state, &memtuples[i]);
			if( (end= memtuples[i].next) < 0 ) break;
		}
		memtuples[i].next= state->mergefreelist;
		state->mergefreelist= old;
	}
	if( new >= 0 )
	{
		int i;
		for(i= new;; i= end)
		{
			CHECK_FOR_INTERRUPTS();
			free_sort_tuple(state, &memtuples[i]);
			if( (end= memtuples[i].next) < 0 ) break;
		}
		memtuples[i].next= state->mergefreelist;
		state->mergefreelist= new;
	}
	return start;
}

/*
 * Sorb initial stage: identify runs.
 * If the new tuple continues the run in progress, link it to the run.
 * Otherwise merge the completed run (sorb 2nd stage) and start a new run.
 */
static inline void
sorb_link_ssup(struct Tuplesortstate * state, int new)
{
	SortTuple *memtuples = state->memtuples;
	SortTuple *np = &memtuples[new];
	SortTuple *op;
	int head = state->runhooks[0];
	int end;
	unsigned bound = state->bounded ? (unsigned)state->bound : UINT_MAX;
	int cmp;
	SortSupport onlyKey = state->onlyKey;

	np->prev = -1;

	if ( head >= 0 )
	{	/* not 1st ever tuple */
		op = &memtuples[head];

		CHECK_FOR_INTERRUPTS();
		if( op->next == -1 )
		{	/* 2nd in run; establishes run direction */
			cmp = cmp_ssup(op, np, onlyKey);
			if( state->dedup  &&  cmp == 0 )
				goto drop;					/* A duplicate; drop the new one */
			else if( (cmp <= 0) )
			{	/* Non-descending order run */
				if( 1 == bound )
					goto drop;				/* run already as long as required; drop the new one */
				else
				{
					state->runlen = 2;
					np->next = -1;
					op->next = state->list_end = new;
				}
			}
			else /* Descending-order run */
			{
				if( 1 == state->bound )
				{	/* run already as long as required; drop the head one */
					CHECK_FOR_INTERRUPTS();
					free_sort_tuple(state, op);
					op->tuple = np->tuple;
					op->datum1 = np->datum1;
					op->isnull1 = np->isnull1;
					np->next = state->mergefreelist;
					state->mergefreelist = new;
				}
				else
				{
					state->runlen = -2;
					np->next = head;
					state->runhooks[0] = new;
					op->prev = new;
				}
			}
			return;	/* 2-element list is on hook 0 */
		}

		/* run direction established */
		if( state->runlen > 0 )
		{	/* Non-descending order run */
			end = state->list_end;
			op = &memtuples[end];
			cmp = cmp_ssup(op, np, onlyKey);
			if( state->dedup  &&  cmp == 0 )
				goto drop;					/* A duplicate; drop the new one */
			else if( (cmp <= 0) )
			{	/* new tuple extends run */
				if( state->runlen >= bound )
					goto drop;				/* run already as long as required; drop the new one */
				else
				{							/* append the new one */
					++state->runlen;
					np->next = -1;
					op->next = state->list_end = new;
				}
				return;	/* >=2 element list, on hook 0 */
			}
		}
		else
		{	/* Descending-order run */
			cmp = cmp_ssup(op, np, onlyKey);
			if( state->dedup  &&  cmp == 0 )
				goto drop;					/* A duplicate; drop the new one */
			else if( (cmp > 0) )
			{	/* new tuple extends run */
				if( -state->runlen >= bound )
				{	/* run already as long as required; drop the largest one & keep the new */
					SortTuple *ep;
					end = state->list_end;
					ep = &memtuples[end];
					CHECK_FOR_INTERRUPTS();
					free_sort_tuple(state, ep);
					ep->next = state->mergefreelist;
					state->mergefreelist = end;
					state->list_end = end = ep->prev;
					ep->next = -1;
				}
				else
					--state->runlen;
											/* prepend the new one */
				np->next = head;
				state->runhooks[0] = new;
				op->prev = new;
				return;	/* >=2 element list, on hook 0 */
			}
		}

		/*
		 * The new tuple breaks the current run.
		 * Merge the run to free hook zero, then place the tuple
		 * as first in a new run onto it.
		 */
{
int i;
if(state->runlen < 0) state->runlen= -state->runlen;
for(i= 1, state->runlen /= 4; state->runlen; state->runlen /= 2) i++;
/* i is now the hook to merge this run at */

/* note this choice makes the sort non-stable; to maintain stability we could
   merge all hooks from 1 to at least i first, then place or merge at i. */
/* Maybe we should have a stability option switch. */

		{
			int hook;
			int start = state->runhooks[0];
			for ( hook = i;
				  state->runhooks[hook] >= 0  &&  hook <= state->maxhook + 1;
				  hook++ )
			{
				start = sorb_merge_ssup(state,
										state->runhooks[hook], start, false);
				state->runhooks[hook] = -1;
			}
			/* all hooks up to "hook" now free; use top one for exp schedule */
			/* or lower to save memory for bounded case */
			if( hook > state->maxhook )
			{
				if ( state->bounded  &&  1<<hook > state->bound - state->bound/4 )
					--hook;
				else
					state->maxhook = hook;
			}
			state->runhooks[hook] = start;
		}
}
	}

	np->next = -1;
	state->runhooks[0] = state->list_end = new;
	return;	/* 1-element list, on hook 0 */

drop:
	/* A duplicate; drop the new one */
	CHECK_FOR_INTERRUPTS();
	free_sort_tuple(state, &memtuples[new]);
	np->next = state->mergefreelist;
	state->mergefreelist = new;
	return;
}
static inline void
sorb_link(struct Tuplesortstate * state, int new)
{
	SortTuple *memtuples = state->memtuples;
	int head = state->runhooks[0];
	int end;
	unsigned bound = state->bounded ? (unsigned)state->bound : UINT_MAX;
	int cmp;

	memtuples[new].prev = -1;

	if ( head >= 0 )
	{	/* not 1st ever tuple */

		CHECK_FOR_INTERRUPTS();
		if( memtuples[head].next == -1 )
		{	/* 2nd in run; establishes run direction */
			cmp = COMPARETUP(state, &memtuples[head], &memtuples[new]);
			if( state->dedup  &&  cmp == 0 )
				goto drop;					/* A duplicate; drop the new one */
			else if( (cmp <= 0) )
			{	/* Non-descending order run */
				state->runlen = 1;
				if( 1 == bound )
					goto drop;				/* run already longer than required; drop the new one */
				else
				{
					memtuples[new].next = -1;
					memtuples[head].next = state->list_end = new;
				}
			}
			else /* Descending-order run */
			{
				if( 1 == state->bound )
				{	/* run already longer than required; drop the head one */
					CHECK_FOR_INTERRUPTS();
					free_sort_tuple(state, &memtuples[head]);
					memtuples[head].tuple = memtuples[new].tuple;
					memtuples[head].datum1 = memtuples[new].datum1;
					memtuples[head].isnull1 = memtuples[new].isnull1;
					memtuples[new].next = state->mergefreelist;
					state->mergefreelist = new;
				}
				else
				{
					state->runlen = -1;
					memtuples[new].next = head;
					state->runhooks[0] = new;
					memtuples[head].prev = new;
				}
			}
			return;	/* 2-element list is on hook 0 */
		}

		/* run direction established */
		if( state->runlen > 0 )
		{	/* Non-descending order run */
			end = state->list_end;
			cmp = COMPARETUP(state, &memtuples[end], &memtuples[new]);
			if( state->dedup  &&  cmp == 0 )
				goto drop;					/* A duplicate; drop the new one */
			else if( (cmp <= 0) )
			{	/* new tuple extends run */
				if( state->runlen >= bound )
					goto drop;				/* run now longer than required; drop the new one */
				else
				{
					++state->runlen;
					memtuples[new].next = -1;
					memtuples[end].next = state->list_end = new;
				}
				return;	/* >=2 element list, on hook 0 */
			}
		}
		else
		{	/* Descending-order run */
			cmp = COMPARETUP(state, &memtuples[head], &memtuples[new]);
			if( state->dedup  &&  cmp == 0 )
				goto drop;					/* A duplicate; drop the new one */
			else if( (cmp > 0) )
			{	/* new tuple extends run */
				if( -state->runlen >= bound )
				{	/* run now longer than required; drop the largest one */
					end = state->list_end;
					free_sort_tuple(state, &memtuples[end]);
					memtuples[end].next = state->mergefreelist;
					state->mergefreelist = end;
					state->list_end = end = memtuples[end].prev;
					memtuples[end].next = -1;
				}
				else
					--state->runlen;

				memtuples[new].next = head;
				state->runhooks[0] = new;
				memtuples[head].prev = new;
				return;	/* >=2 element list, on hook 0 */
			}
		}

		/*
		 * The new tuple breaks the current run.
		 * Merge the run to free hook zero, then place the tuple
		 * as first in a new run onto it.
		 */
		{
			int hook;
			int start = state->runhooks[0];
			for ( hook = 1;
				  state->runhooks[hook] >= 0  &&  hook <= state->maxhook + 1;
				  hook++ )
			{
				start = sorb_merge(state, state->runhooks[hook], start, false);
				state->runhooks[hook] = -1;
			}
			/* all hooks up to "hook" now free; use top one for exp schedule */
			/* or lower to save memory for bounded case */
			if( hook > state->maxhook )
			{
				if ( state->bounded  &&  1<<hook > state->bound - state->bound/4 )
					--hook;
				else
					state->maxhook = hook;
			}
			state->runhooks[hook] = start;
		}
	}

	memtuples[new].next = -1;
	state->runhooks[0] = state->list_end = new;
	return;	/* 1-element list, on hook 0 */

drop:
	/* A duplicate; drop the new one */
	CHECK_FOR_INTERRUPTS();
	free_sort_tuple(state, &memtuples[new]);
	memtuples[new].next = state->mergefreelist;
	state->mergefreelist = new;
	return;
}

/* Third-stage of sorb: merge intermediate runs to final list. */
/* If asked, build a backlink chain during the active merge portion */
/* of the final merge.												*/
static int
sorb_collector_ssup(struct Tuplesortstate * state, bool backlink)
{
	int hook = 0;
	int start;

	state->list_end = -1;
	state->reverse_linkage = false;
	if( state->memtupcount == 0 )
		return -1;

	while( state->runhooks[hook] == -1 )
		hook++;
	start = state->runhooks[hook];
	while( ++hook < state->maxhook )
		if( state->runhooks[hook] >= 0 )
			start = sorb_merge_ssup(state, state->runhooks[hook], start, false);
	if( state->runhooks[hook] >= 0 )
		start = sorb_merge_ssup(state, state->runhooks[hook], start, backlink);
	return start;
}
static int
sorb_collector(struct Tuplesortstate * state, bool backlink)
{
	int hook = 0;
	int start;

	state->list_end = -1;
	state->reverse_linkage = false;
	if( state->memtupcount == 0 )
		return -1;

	while( state->runhooks[hook] == -1 )
		hook++;
	start = state->runhooks[hook];
	while( ++hook < state->maxhook )
		if( state->runhooks[hook] >= 0 )
			start = sorb_merge(state, state->runhooks[hook], start, false);
	if( state->runhooks[hook] >= 0 )
		start = sorb_merge(state, state->runhooks[hook], start, backlink);
	return start;
}

/* Do the whole sorb thing on an already-loaded array */
/*XXX could we use the pointers implementation?  Does randomAccess say no? */
/* Leave for now; all the randomAccess routines would need looking at; this
   is probably the only gotcha beyond making the SortTuple a union */
static int
sorb_oneshot(struct Tuplesortstate * state)
{
	SortTuple *memtuples = state->memtuples;
	int item;
	SortTuple *np;
	SortTuple *hp;	/* head of run */
	int hi;
	SortTuple *ep;	/* end of run */
	const int lim = state->memtupcount;
	const SortSupport onlyKey = state->onlyKey;
	int runlen;

	/* No bounds or dedup; TSS_SORB is used for them */

	/* Linker, with scheduled partial merges */
	item = 0, np = memtuples;
	while(item < lim)
	{
		hi = item, hp = ep = np, runlen = 1;

		if(++item < lim)
		{
			CHECK_FOR_INTERRUPTS();
			np++;
			if((onlyKey ? cmp_ssup(hp, np, onlyKey)
					    : COMPARETUP(state, hp, np)) <= 0)
				do			/* non-descending order run */
				{			/* append to list */
					CHECK_FOR_INTERRUPTS();
					ep->next = item, runlen++;
					ep = np;
					if(++item >= lim) break;
					np++;
				} while((onlyKey ? cmp_ssup(ep, np, onlyKey)
								 : COMPARETUP(state, ep, np)) <= 0);
			else			
				do			/* descending-order run */
				{			/* prepend to list */
					CHECK_FOR_INTERRUPTS();
					np->next = hi, runlen++;
					hp = np, hi = item;
					if(++item >= lim) break;
					np++;
				} while((onlyKey ? cmp_ssup(hp, np, onlyKey)
								 : COMPARETUP(state, hp, np)) > 0);
			/* item, np breaks run (or past eof) */
			/* hi is now the head of a non-descending run */
		}
		ep->next = -1;

		/* Merge run onto hooks starting at optimal placement point.
		 * This gives 1.6% fewer compares at N=2e5 random, compared to
		 * always starting at the bottom.
		 */
/* note this choice makes the sort non-stable; to maintain stability we could
   merge all hooks from 0 to at least here first, then place or merge here. */
/* Maybe we should have a stability option switch. */
		{
			int hook;
			for ( hook= 0, runlen /= 4; runlen; runlen /= 2 ) hook++;
			for ( ;
				  state->runhooks[hook] >= 0  &&  hook <= state->maxhook + 1;
				  hook++ )
			{
				hi= onlyKey ? sorb_merge_basic(state, state->runhooks[hook], hi)
							: sorb_merge(state, state->runhooks[hook], hi, false);
				state->runhooks[hook] = -1;
			}
			/* all hooks up to "hook" now free; use top one for exp schedule */
			/* or lower to save memory for bounded case */
			if( hook > state->maxhook )
			{
				if ( state->bounded  &&  1<<hook > state->bound - state->bound/4 )
					--hook;
				else
					state->maxhook = hook;
			}
			state->runhooks[hook] = hi;
		}
		/* start over with the run-breaking item */
	}

	/* Collector */
	return onlyKey ? sorb_collector_ssup(state, state->randomAccess)
				   : sorb_collector(state, state->randomAccess);
}

/*
 * Walk the list setting up a back-link chain (for reverse access).
 */
static inline void
sorb_reverse_chain(struct Tuplesortstate * state, int start)
{
	int this, next;

	Assert(start >= 0);
	Assert(state->memtuples[start].prev == -1);

	for( this = state->list_end >= 0 ? state->list_end : start;
		 (next = state->memtuples[this].next) >= 0;
		 this = next)
		state->memtuples[next].prev = this;
	state->list_end = this;
	state->reverse_linkage = true;
}

/*
 * Compare two SortTuples.	If checkIndex is true, use the tuple index
 * as the front of the sort key; otherwise, no.
 */

#define HEAPCOMPARE(tup1,tup2) \
	(checkIndex && ((tup1)->tupindex != (tup2)->tupindex) ? \
	 ((tup1)->tupindex) - ((tup2)->tupindex) : \
	 COMPARETUP(state, tup1, tup2))

/*
 * Convert the sorted-list output of a sorb run, sparsely resident in
 * memtuples[], in-place into a heap.  Each tuple is marked as belonging
 * to run number zero.
 *
 * Walk the list once setting up a back-link chain (for ease of swaps),
 * then again swapping each element into a sorted array order; this is
 * trivially a well-formed heap. N-1 swaps plus a move are needed
 * (none at all for the presorted-input case).
 */
static inline void
heapify_sorted_list(struct Tuplesortstate * state, int start)
{
	SortTuple * memtuples = state->memtuples;
	int j;		/* pos in heap array */
	int i;		/* element in sorted-list */
	SortTuple * this;	/* ditto */
	int next;
	SortTuple * dest;	/* element in heap */
	int ntuples = state->memtupcount;

	if( ntuples == 0 )
		return;					/* no work to do */

	/* Build backlink chain, accounting for work already done in sorb_merge */
	memtuples[start].prev = -1;
	sorb_reverse_chain(state, start);

	/* Walk list, copying to an in-order array */
	for( i= start, j= 0, next = memtuples[i].next;
		 next >= 0;
		 i= next, next= memtuples[next].next, j++)
	{
		SortTuple	tmp;

		Assert(i >= j);					/* item should not yet be in heap */
		this = &memtuples[i];			/* old location in list */
		if( i == j )
		{								/* already in right place for heap */
			this->tupindex = 0;
			memtuples[next].prev= -1;	/* "next" is now head of list */
			continue;
		}
		dest = &memtuples[j];			/* new location in heap */
		tmp = *dest;					/* make space for new heap element */

		dest->tuple = this->tuple;		/* move element to heap...	*/
		dest->datum1 = this->datum1;	/*							*/
		dest->isnull1 = this->isnull1;	/*							*/
		dest->tupindex = 0;				/* ... setting run number 0	*/

		*this = tmp;					/* element displaced by heap */

		if( next >= j )
		{
			if( next == j )
			{
				tmp.prev= -1;		/* cached "next" */
				next = i;
			}
			memtuples[next].prev= -1;	/* "next" is now head of list */
		}

		if(tmp.prev > j)				/* ... has prev not in heap  */
			memtuples[tmp.prev].next = i; /* change to new loc */
		if(tmp.next > j)				/* ... has succ not in heap  */
			memtuples[tmp.next].prev = i; /* change to new loc */
	}

	dest = &memtuples[j];		/* new location in heap */
	if( i != j )
	{
		this = &memtuples[i];			/* old location in list */
		dest->tuple = this->tuple;		/* move element to heap...	*/
		dest->datum1 = this->datum1;	/*							*/
		dest->isnull1 = this->isnull1;	/*							*/
	}
	dest->tupindex = 0;					/* ... setting run number 0	*/

	state->memtupcount = j+1;
	Assert(state->memtupcount <= ntuples);	/* can be smaller by size of freelist */
}

/*
 * Convert the unsorted contents of memtuples[] into a heap. Each tuple is
 * marked as belonging to run number zero.
 *
 * NOTE: we pass false for checkIndex since there's no point in comparing
 * indexes in this step, even though we do intend the indexes to be part
 * of the sort key...
 */
static inline void
heapify_unsorted_array(Tuplesortstate * state)
{
	int j;
	int ntuples = state->memtupcount;
	state->memtupcount = 0;		/* make the heap empty */
	for (j = 0; j < ntuples; j++)
	{
		/* Must copy source tuple to avoid possible overwrite */
		SortTuple	stup = state->memtuples[j];

		tuplesort_heap_insert(state, &stup, 0, false);
	}
	Assert(state->memtupcount == ntuples);
}

/*
 * Remove tuples from sorted list and write to tape.
 *
 * This is used when converting from sorb to tape-operations.
 * Dump enough tuples to get under the availMem limit (but
 * always leave one tuple), and to have at least one free slot
 * in the memtuples[] array>
 *
 * We reduce both the memory-used and slots-used info, but do
 * not shuffle the memtuples array down - so on exit the
 * memtupcount does NOT indicate the first free entry.
 * Return the head of the remaining list.
 */
static int
dumptuples_from_list(Tuplesortstate * state, int start)
{
	while ((LACKMEM(state) && state->memtupcount > 1) ||
			state->memtupcount >= state->memtupsize)
	{
		/* Dump the list's frontmost entry. */
		SortTuple * tp = &state->memtuples[start];

		if (start == state->list_end)	/* Backlink work done in    */
			state->list_end = -1;		/* sorb_merge() now useless */

		start = tp->next;
		Assert(state->memtupcount > 0);
		WRITETUP(state, state->tp_tapenum[state->destTape], tp);
		--state->memtupcount;
	}
	return start;
}

/*
 * Shared code for tuple and datum cases.
 */
static void
puttuple_common(Tuplesortstate *state, SortTuple *tuple)
{
	switch (state->status)
	{
		case TSS_SORB:
		{
			int item;
			/*
			 * Save the tuple into the unsorted array.	First, grow the array
			 * as needed.  Note that we try to grow the array when there is
			 * still one free slot remaining --- if we fail, there'll still be
			 * room to store the incoming tuple, and then we'll switch to
			 * tape-based operation.
			 *   Next, unlike the traditional quicksort-based implementation
			 * in TSS_INITIAL, we do some of the sorting work here on input
			 * of each tuple.  First stage identifies runs of non-descending
			 * or descending key values and links them as a non-descending list.
			 * Runs may be pretty short, but at least two items.  Second stage,
			 * also done here, merges pairs of lists in a schedule that on
			 * average operates on equal-length lists.
			 *   The final stage, done when either we run out of input
			 * or out of memory (and must change to an external sort) completes
			 * the merging of all in-progress merge lists. The final list is
			 * then walkable in sorted order.
			 */
			if ((item= state->mergefreelist) >= 0)
			{
				state->mergefreelist = state->memtuples[item].next;
				state->memtuples[item] = *tuple;
			}
			else
			{
				if (state->memtupcount >= state->memtupsize - 1)
				{
					(void) grow_memtuples(state);
					Assert(state->memtupcount < state->memtupsize);
				}
				state->memtuples[(item = state->memtupcount++)] = *tuple;
			}

			/* 1st & 2nd sorb stages */
			/* Can we use the single-key sort function? */
			if (state->onlyKey != NULL)
				sorb_link_ssup(state, item);
			else
				sorb_link(state, item);

			/*
			 * Done if we still fit in available memory and have array slots,
			 * and we're not out of runhooks for sorb intermediate lists.
			 */
			if (  state->memtupcount < state->memtupsize
			   && !LACKMEM(state)
			   && state->maxhook < nelements(state->runhooks)
			   )
				break;

			/*
			 * Nope; time to switch to tape-based operation.
			 * 3rd (final) sorb stage: merge all intermediate lists to one.
			 */
			/* Can we use the single-key sort function? */
			state->list_start = state->onlyKey != NULL
				? sorb_collector_ssup(state, true)
				: sorb_collector(state, true);

			/*
			 * Set up for tape-based operation.
			 * Moves state to TSS_BUILDRUNS.
			 */
			inittapes(state);

			/*
			 * The inittapes allocated some memory for tape buffers.
			 * Dump tuples until we are back under the limit.
			 */
			state->list_start = dumptuples_from_list(state, state->list_start);

			/*
			 * Convert the remaining contents of memtuple[] into a heap. Each
			 * tuple is marked as belonging to run number zero. The conversion
			 * does not depend on memtupcount for a memtuple[] endmark (only
			 * as a valid count) but returns with it valid in both senses.
			 */
			heapify_sorted_list(state, state->list_start);
			break;
		}

		case TSS_INITIAL:

			/*
			 * Save the tuple into the unsorted array.	First, grow the array
			 * as needed.  Note that we try to grow the array when there is
			 * still one free slot remaining --- if we fail, there'll still be
			 * room to store the incoming tuple, and then we'll switch to
			 * tape-based operation.
			 */
			if (state->memtupcount >= state->memtupsize - 1)
			{
				(void) grow_memtuples(state);
				Assert(state->memtupcount < state->memtupsize);
			}
			state->memtuples[state->memtupcount++] = *tuple;

			/*
			 * Check if it's time to switch over to a bounded heapsort. We do
			 * so if the input tuple count exceeds twice the desired tuple
			 * count (this is a heuristic for where heapsort becomes cheaper
			 * than a quicksort), or if we've just filled workMem and have
			 * enough tuples to meet the bound.
			 *
			 * Note that once we enter TSS_BOUNDED state we will always try to
			 * complete the sort that way.	In the worst case, if later input
			 * tuples are larger than earlier ones, this might cause us to
			 * exceed workMem significantly.
			 */
			if (state->bounded &&
				(state->memtupcount > state->bound * 2 ||
				 (state->memtupcount > state->bound && LACKMEM(state))))
			{
#ifdef TRACE_SORT
				if (trace_sort)
					elog(LOG, "switching to bounded heapsort at %d tuples: %s",
						 state->memtupcount,
						 pg_rusage_show(&state->ru_start));
#endif
				make_bounded_heap(state);
				return;
			}

			/*
			 * Done if we still fit in available memory and have array slots.
			 */
			if (state->memtupcount < state->memtupsize && !LACKMEM(state))
				return;

			/*
			 * Nope; time to switch to tape-based operation.
			 */
			inittapes(state);

			/*
			 * Convert the current contents of memtuple[] into a heap. Each tuple is
			 * marked as belonging to run number zero.
			 */
			heapify_unsorted_array(state);

			/*
			 * Dump tuples until we are back under the limit.
			 */
			dumptuples(state, false);
			break;

		case TSS_BOUNDED:

			/*
			 * We don't want to grow the array here, so check whether the new
			 * tuple can be discarded before putting it in.  This should be a
			 * good speed optimization, too, since when there are many more
			 * input tuples than the bound, most input tuples can be discarded
			 * with just this one comparison.  Note that because we currently
			 * have the sort direction reversed, we must check for <= not >=.
			 */
			if (COMPARETUP(state, tuple, &state->memtuples[0]) <= 0)
			{
				/* new tuple <= top of the heap, so we can discard it */
				free_sort_tuple(state, tuple);
				CHECK_FOR_INTERRUPTS();
			}
			else
			{
				/* discard top of heap, sift up, insert new tuple */
				free_sort_tuple(state, &state->memtuples[0]);
				tuplesort_heap_siftup(state, false);
				tuplesort_heap_insert(state, tuple, 0, false);
			}
			break;

		case TSS_BUILDRUNS:

			/*
			 * Insert the tuple into the heap, with run number currentRun if
			 * it can go into the current run, else run number currentRun+1.
			 * The tuple can go into the current run if it is >= the first
			 * not-yet-output tuple.  (Actually, it could go into the current
			 * run if it is >= the most recently output tuple ... but that
			 * would require keeping around the tuple we last output, and it's
			 * simplest to let writetup free each tuple as soon as it's
			 * written.)
			 *
			 * Note there will always be at least one tuple in the heap at
			 * this point; see dumptuples.
			 */
			Assert(state->memtupcount > 0);
			if (COMPARETUP(state, tuple, &state->memtuples[0]) >= 0)
				tuplesort_heap_insert(state, tuple, state->currentRun, true);
			else
				tuplesort_heap_insert(state, tuple, state->currentRun + 1, true);

			/*
			 * If we are over the memory limit, dump tuples till we're under.
			 */
			dumptuples(state, false);
			break;

		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}
}

/*
 * All tuples have been provided; finish the sort.
 */
void
tuplesort_performsort(Tuplesortstate *state)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "performsort starting: %s",
			 pg_rusage_show(&state->ru_start));
#endif

	switch (state->status)
	{
		case TSS_SORB:
			/* 3rd (final) sorb stage: merge all intermediate lists to one */
			/* Can we use the single-key sort function? */
			state->current =
			state->markpos_offset =
			state->list_start = state->onlyKey != NULL
				? sorb_collector_ssup(state, state->randomAccess)
				: sorb_collector(state, state->randomAccess);

			state->eof_reached = false;
			state->markpos_eof = false;

			break;

		case TSS_INITIAL:

			/*
			 * We were able to accumulate all the tuples within the allowed
			 * amount of memory.  Just qsort 'em and we're done.
			 */
			if (state->memtupcount > 1)
			{
				if (enable_intmerge_sort)
				{
					state->status = TSS_SORB_ONESHOT;
					state->current =
					state->markpos_offset =
					state->list_start= sorb_oneshot(state);
					break;
				}

				/* Can we use the single-key sort function? */
				if (state->onlyKey != NULL)
					qsort_ssup(state->memtuples, state->memtupcount,
							   state->onlyKey);
				else
					qsort_tuple(state->memtuples,
								state->memtupcount,
								state->comparetup,
								state);
			}
			state->current = 0;
			state->eof_reached = false;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			state->status = TSS_SORTEDINMEM;
			break;

		case TSS_BOUNDED:

			/*
			 * We were able to accumulate all the tuples required for output
			 * in memory, using a heap to eliminate excess tuples.	Now we
			 * have to transform the heap to a properly-sorted array.
			 */
			sort_bounded_heap(state);
			state->current = 0;
			state->eof_reached = false;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			state->status = TSS_SORTEDINMEM;
			break;

		case TSS_BUILDRUNS:

			/*
			 * Finish tape-based sort.	First, flush all tuples remaining in
			 * memory out to tape; then merge until we have a single remaining
			 * run (or, if !randomAccess, one run per tape). Note that
			 * mergeruns sets the correct state->status.
			 */
			dumptuples(state, true);
			mergeruns(state);
			state->eof_reached = false;
			state->markpos_block = 0L;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;

		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}

#ifdef TRACE_SORT
	if (trace_sort)
	{
		if (state->status == TSS_FINALMERGE)
			elog(LOG, "performsort done (except %d-way final merge): %s",
				 state->activeTapes,
				 pg_rusage_show(&state->ru_start));
		else
			elog(LOG, "performsort done:     %s",
				 pg_rusage_show(&state->ru_start));
	}
#endif

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Internal routine to fetch the next tuple in either forward or back
 * direction into *stup.  Returns FALSE if no more tuples.
 * If *should_free is set, the caller must pfree stup.tuple when done with it.
 */
static bool
tuplesort_gettuple_common(Tuplesortstate *state, bool forward,
						  SortTuple *stup, bool *should_free)
{
	unsigned int tuplen;

	switch (state->status)
	{
		case TSS_SORB:
		case TSS_SORB_ONESHOT:
			Assert(forward || state->randomAccess);
			*should_free = false;
			if (forward)
			{
				if (state->current >= 0)
				{
					SortTuple tup = state->memtuples[state->current];
					*stup = tup;
					state->current = tup.next;
					return true;
				}

				/* No obvious way to check for over-read bounded sort. */
				/* Also, the placement of the test in TSS_SORTEDINMEM  */
				/* looks entirely wrong.							   */

				state->eof_reached = true;
				return false;
			}
			else
			{
				int item = state->current;

				if (item == state->list_start)
					return false;

				/* Build backlinks, if not already done */
				if (!state->reverse_linkage)
					sorb_reverse_chain(state, state->list_start);

				/*
				 * if all tuples are fetched already then we return last
				 * tuple, else - tuple before last returned.
				 */
				if (state->eof_reached)
					state->eof_reached = false;
				else
				{
					item = state->current = state->memtuples[item].prev;
					if (item == state->list_start)
						return false;
				}
				item = state->memtuples[item].prev;
				*stup = state->memtuples[item];
				return true;
			}
			break;

		case TSS_SORTEDINMEM:
			Assert(forward || state->randomAccess);
			*should_free = false;
			if (forward)
			{
				if (state->current < state->memtupcount)
				{
					*stup = state->memtuples[state->current++];
					return true;
				}
				state->eof_reached = true;

				/*
				 * Complain if caller tries to retrieve more tuples than
				 * originally asked for in a bounded sort.	This is because
				 * returning EOF here might be the wrong thing.
				 */
				if (state->bounded && state->current >= state->bound)
					elog(ERROR, "retrieved too many tuples in a bounded sort");

				return false;
			}
			else
			{
				if (state->current <= 0)
					return false;

				/*
				 * if all tuples are fetched already then we return last
				 * tuple, else - tuple before last returned.
				 */
				if (state->eof_reached)
					state->eof_reached = false;
				else
				{
					state->current--;	/* last returned tuple */
					if (state->current <= 0)
						return false;
				}
				*stup = state->memtuples[state->current - 1];
				return true;
			}
			break;

		case TSS_SORTEDONTAPE:
			Assert(forward || state->randomAccess);
			*should_free = true;
			if (forward)
			{
				if (state->eof_reached)
					return false;
				if ((tuplen = getlen(state, state->result_tape, true)) != 0)
				{
					READTUP(state, stup, state->result_tape, tuplen);
					return true;
				}
				else
				{
					state->eof_reached = true;
					return false;
				}
			}

			/*
			 * Backward.
			 *
			 * if all tuples are fetched already then we return last tuple,
			 * else - tuple before last returned.
			 */
			if (state->eof_reached)
			{
				/*
				 * Seek position is pointing just past the zero tuplen at the
				 * end of file; back up to fetch last tuple's ending length
				 * word.  If seek fails we must have a completely empty file.
				 */
				if (!LogicalTapeBackspace(state->tapeset,
										  state->result_tape,
										  2 * sizeof(unsigned int)))
					return false;
				state->eof_reached = false;
			}
			else
			{
				/*
				 * Back up and fetch previously-returned tuple's ending length
				 * word.  If seek fails, assume we are at start of file.
				 */
				if (!LogicalTapeBackspace(state->tapeset,
										  state->result_tape,
										  sizeof(unsigned int)))
					return false;
				tuplen = getlen(state, state->result_tape, false);

				/*
				 * Back up to get ending length word of tuple before it.
				 */
				if (!LogicalTapeBackspace(state->tapeset,
										  state->result_tape,
										  tuplen + 2 * sizeof(unsigned int)))
				{
					/*
					 * If that fails, presumably the prev tuple is the first
					 * in the file.  Back up so that it becomes next to read
					 * in forward direction (not obviously right, but that is
					 * what in-memory case does).
					 */
					if (!LogicalTapeBackspace(state->tapeset,
											  state->result_tape,
											  tuplen + sizeof(unsigned int)))
						elog(ERROR, "bogus tuple length in backward scan");
					return false;
				}
			}

			tuplen = getlen(state, state->result_tape, false);

			/*
			 * Now we have the length of the prior tuple, back up and read it.
			 * Note: READTUP expects we are positioned after the initial
			 * length word of the tuple, so back up to that point.
			 */
			if (!LogicalTapeBackspace(state->tapeset,
									  state->result_tape,
									  tuplen))
				elog(ERROR, "bogus tuple length in backward scan");
			READTUP(state, stup, state->result_tape, tuplen);
			return true;

		case TSS_FINALMERGE:
			Assert(forward);
			*should_free = true;

			/*
			 * This code should match the inner loop of mergeonerun().
			 */
			if (state->memtupcount > 0)
			{
				int			srcTape = state->memtuples[0].tupindex;
				Size		tuplen;
				int			tupIndex;
				SortTuple  *newtup;

				*stup = state->memtuples[0];
				/* returned tuple is no longer counted in our memory space */
				if (stup->tuple)
				{
					tuplen = GetMemoryChunkSpace(stup->tuple);
					state->availMem += tuplen;
					state->mergeavailmem[srcTape] += tuplen;
				}
				tuplesort_heap_siftup(state, false);
				if ((tupIndex = state->mergenext[srcTape]) == 0)
				{
					/*
					 * out of preloaded data on this tape, try to read more
					 *
					 * Unlike mergeonerun(), we only preload from the single
					 * tape that's run dry.  See mergepreread() comments.
					 */
					mergeprereadone(state, srcTape);

					/*
					 * if still no data, we've reached end of run on this tape
					 */
					if ((tupIndex = state->mergenext[srcTape]) == 0)
						return true;
				}
				/* pull next preread tuple from list, insert in heap */
				newtup = &state->memtuples[tupIndex];
				state->mergenext[srcTape] = newtup->tupindex;
				if (state->mergenext[srcTape] == 0)
					state->mergelast[srcTape] = 0;
				tuplesort_heap_insert(state, newtup, srcTape, false);
				/* put the now-unused memtuples entry on the freelist */
				newtup->tupindex = state->mergefreelist;
				state->mergefreelist = tupIndex;
				state->mergeavailslots[srcTape]++;
				return true;
			}
			return false;

		default:
			elog(ERROR, "invalid tuplesort state");
			return false;		/* keep compiler quiet */
	}
}

/*
 * Fetch the next tuple in either forward or back direction.
 * If successful, put tuple in slot and return TRUE; else, clear the slot
 * and return FALSE.
 */
bool
tuplesort_gettupleslot(Tuplesortstate *state, bool forward,
					   TupleTableSlot *slot)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;
	bool		should_free;

	if (!tuplesort_gettuple_common(state, forward, &stup, &should_free))
		stup.tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	if (stup.tuple)
	{
		ExecStoreMinimalTuple((MinimalTuple) stup.tuple, slot, should_free);
		return true;
	}
	else
	{
		ExecClearTuple(slot);
		return false;
	}
}

/*
 * Fetch the next tuple in either forward or back direction.
 * Returns NULL if no more tuples.	If *should_free is set, the
 * caller must pfree the returned tuple when done with it.
 */
HeapTuple
tuplesort_getheaptuple(Tuplesortstate *state, bool forward, bool *should_free)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup, should_free))
		stup.tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	return stup.tuple;
}

/*
 * Fetch the next index tuple in either forward or back direction.
 * Returns NULL if no more tuples.	If *should_free is set, the
 * caller must pfree the returned tuple when done with it.
 */
IndexTuple
tuplesort_getindextuple(Tuplesortstate *state, bool forward,
						bool *should_free)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup, should_free))
		stup.tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	return (IndexTuple) stup.tuple;
}

/*
 * Fetch the next Datum in either forward or back direction.
 * Returns FALSE if no more datums.
 *
 * If the Datum is pass-by-ref type, the returned value is freshly palloc'd
 * and is now owned by the caller.
 */
bool
tuplesort_getdatum(Tuplesortstate *state, bool forward,
				   Datum *val, bool *isNull)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);
	SortTuple	stup;
	bool		should_free;

	if (!tuplesort_gettuple_common(state, forward, &stup, &should_free))
	{
		MemoryContextSwitchTo(oldcontext);
		return false;
	}

	if (stup.isnull1 || state->datumTypeByVal)
	{
		*val = stup.datum1;
		*isNull = stup.isnull1;
	}
	else
	{
		if (should_free)
			*val = stup.datum1;
		else
			*val = datumCopy(stup.datum1, false, state->datumTypeLen);
		*isNull = false;
	}

	MemoryContextSwitchTo(oldcontext);

	return true;
}

/*
 * tuplesort_merge_order - report merge order we'll use for given memory
 * (note: "merge order" just means the number of input tapes in the merge).
 *
 * This is exported for use by the planner.  allowedMem is in bytes.
 */
int
tuplesort_merge_order(int64 allowedMem)
{
	int			mOrder;

	/*
	 * We need one tape for each merge input, plus another one for the output,
	 * and each of these tapes needs buffer space.	In addition we want
	 * MERGE_BUFFER_SIZE workspace per input tape (but the output tape doesn't
	 * count).
	 *
	 * Note: you might be thinking we need to account for the memtuples[]
	 * array in this calculation, but we effectively treat that as part of the
	 * MERGE_BUFFER_SIZE workspace.
	 */
	mOrder = (allowedMem - TAPE_BUFFER_OVERHEAD) /
		(MERGE_BUFFER_SIZE + TAPE_BUFFER_OVERHEAD);

	/* Even in minimum memory, use at least a MINORDER merge */
	mOrder = Max(mOrder, MINORDER);

	return mOrder;
}


/*
 * inittapes - initialize for tape sorting.
 *
 * This is called only if we have found we don't have room to sort in memory.
 */
static void
inittapes(Tuplesortstate *state)
{
	int			maxTapes,
				j;
	int64		tapeSpace;

	/* Compute number of tapes to use: merge order plus 1 */
	maxTapes = tuplesort_merge_order(state->allowedMem) + 1;

	/*
	 * We must have at least 2*maxTapes slots in the memtuples[] array, else
	 * we'd not have room for merge heap plus preread.  It seems unlikely that
	 * this case would ever occur, but be safe.
	 */
	maxTapes = Min(maxTapes, state->memtupsize / 2);

	state->maxTapes = maxTapes;
	state->tapeRange = maxTapes - 1;

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "switching to external sort with %d tapes: %s",
			 maxTapes, pg_rusage_show(&state->ru_start));
#endif

	/*
	 * Decrease availMem to reflect the space needed for tape buffers; but
	 * don't decrease it to the point that we have no room for tuples. (That
	 * case is only likely to occur if sorting pass-by-value Datums; in all
	 * other scenarios the memtuples[] array is unlikely to occupy more than
	 * half of allowedMem.	In the pass-by-value case it's not important to
	 * account for tuple space, so we don't care if LACKMEM becomes
	 * inaccurate.)
	 */
	tapeSpace = (int64) maxTapes * TAPE_BUFFER_OVERHEAD;
	if (tapeSpace + GetMemoryChunkSpace(state->memtuples) < state->allowedMem)
		USEMEM(state, tapeSpace);

	/*
	 * Make sure that the temp file(s) underlying the tape set are created in
	 * suitable temp tablespaces.
	 */
	PrepareTempTablespaces();

	/*
	 * Create the tape set and allocate the per-tape data arrays.
	 */
	state->tapeset = LogicalTapeSetCreate(maxTapes);

	state->mergeactive = (bool *) palloc0(maxTapes * sizeof(bool));
	state->mergenext = (int *) palloc0(maxTapes * sizeof(int));
	state->mergelast = (int *) palloc0(maxTapes * sizeof(int));
	state->mergeavailslots = (int *) palloc0(maxTapes * sizeof(int));
	state->mergeavailmem = (int64 *) palloc0(maxTapes * sizeof(int64));
	state->tp_fib = (int *) palloc0(maxTapes * sizeof(int));
	state->tp_runs = (int *) palloc0(maxTapes * sizeof(int));
	state->tp_dummy = (int *) palloc0(maxTapes * sizeof(int));
	state->tp_tapenum = (int *) palloc0(maxTapes * sizeof(int));

	state->currentRun = 0;

	/*
	 * Initialize variables of Algorithm D (step D1).
	 */
	for (j = 0; j < maxTapes; j++)
	{
		state->tp_fib[j] = 1;
		state->tp_runs[j] = 0;
		state->tp_dummy[j] = 1;
		state->tp_tapenum[j] = j;
	}
	state->tp_fib[state->tapeRange] = 0;
	state->tp_dummy[state->tapeRange] = 0;

	state->Level = 1;
	state->destTape = 0;

	state->status = TSS_BUILDRUNS;
}

/*
 * selectnewtape -- select new tape for new initial run.
 *
 * This is called after finishing a run when we know another run
 * must be started.  This implements steps D3, D4 of Algorithm D.
 */
static void
selectnewtape(Tuplesortstate *state)
{
	int			j;
	int			a;

	/* Step D3: advance j (destTape) */
	if (state->tp_dummy[state->destTape] < state->tp_dummy[state->destTape + 1])
	{
		state->destTape++;
		return;
	}
	if (state->tp_dummy[state->destTape] != 0)
	{
		state->destTape = 0;
		return;
	}

	/* Step D4: increase level */
	state->Level++;
	a = state->tp_fib[0];
	for (j = 0; j < state->tapeRange; j++)
	{
		state->tp_dummy[j] = a + state->tp_fib[j + 1] - state->tp_fib[j];
		state->tp_fib[j] = a + state->tp_fib[j + 1];
	}
	state->destTape = 0;
}

/*
 * mergeruns -- merge all the completed initial runs.
 *
 * This implements steps D5, D6 of Algorithm D.  All input data has
 * already been written to initial runs on tape (see dumptuples).
 */
static void
mergeruns(Tuplesortstate *state)
{
	int			tapenum,
				svTape,
				svRuns,
				svDummy;

	Assert(state->status == TSS_BUILDRUNS);
	Assert(state->memtupcount == 0);

	/*
	 * If we produced only one initial run (quite likely if the total data
	 * volume is between 1X and 2X workMem), we can just use that tape as the
	 * finished output, rather than doing a useless merge.	(This obvious
	 * optimization is not in Knuth's algorithm.)
	 */
	if (state->currentRun == 1)
	{
		state->result_tape = state->tp_tapenum[state->destTape];
		/* must freeze and rewind the finished output tape */
		LogicalTapeFreeze(state->tapeset, state->result_tape);
		state->status = TSS_SORTEDONTAPE;
		return;
	}

	/* End of step D2: rewind all output tapes to prepare for merging */
	for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
		LogicalTapeRewind(state->tapeset, tapenum, false);

	for (;;)
	{
		/*
		 * At this point we know that tape[T] is empty.  If there's just one
		 * (real or dummy) run left on each input tape, then only one merge
		 * pass remains.  If we don't have to produce a materialized sorted
		 * tape, we can stop at this point and do the final merge on-the-fly.
		 */
		if (!state->randomAccess)
		{
			bool		allOneRun = true;

			Assert(state->tp_runs[state->tapeRange] == 0);
			for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
			{
				if (state->tp_runs[tapenum] + state->tp_dummy[tapenum] != 1)
				{
					allOneRun = false;
					break;
				}
			}
			if (allOneRun)
			{
				/* Tell logtape.c we won't be writing anymore */
				LogicalTapeSetForgetFreeSpace(state->tapeset);
				/* Initialize for the final merge pass */
				beginmerge(state);
				state->status = TSS_FINALMERGE;
				return;
			}
		}

		/* Step D5: merge runs onto tape[T] until tape[P] is empty */
		while (state->tp_runs[state->tapeRange - 1] ||
			   state->tp_dummy[state->tapeRange - 1])
		{
			bool		allDummy = true;

			for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
			{
				if (state->tp_dummy[tapenum] == 0)
				{
					allDummy = false;
					break;
				}
			}

			if (allDummy)
			{
				state->tp_dummy[state->tapeRange]++;
				for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
					state->tp_dummy[tapenum]--;
			}
			else
				mergeonerun(state);
		}

		/* Step D6: decrease level */
		if (--state->Level == 0)
			break;
		/* rewind output tape T to use as new input */
		LogicalTapeRewind(state->tapeset, state->tp_tapenum[state->tapeRange],
						  false);
		/* rewind used-up input tape P, and prepare it for write pass */
		LogicalTapeRewind(state->tapeset, state->tp_tapenum[state->tapeRange - 1],
						  true);
		state->tp_runs[state->tapeRange - 1] = 0;

		/*
		 * reassign tape units per step D6; note we no longer care about A[]
		 */
		svTape = state->tp_tapenum[state->tapeRange];
		svDummy = state->tp_dummy[state->tapeRange];
		svRuns = state->tp_runs[state->tapeRange];
		for (tapenum = state->tapeRange; tapenum > 0; tapenum--)
		{
			state->tp_tapenum[tapenum] = state->tp_tapenum[tapenum - 1];
			state->tp_dummy[tapenum] = state->tp_dummy[tapenum - 1];
			state->tp_runs[tapenum] = state->tp_runs[tapenum - 1];
		}
		state->tp_tapenum[0] = svTape;
		state->tp_dummy[0] = svDummy;
		state->tp_runs[0] = svRuns;
	}

	/*
	 * Done.  Knuth says that the result is on TAPE[1], but since we exited
	 * the loop without performing the last iteration of step D6, we have not
	 * rearranged the tape unit assignment, and therefore the result is on
	 * TAPE[T].  We need to do it this way so that we can freeze the final
	 * output tape while rewinding it.	The last iteration of step D6 would be
	 * a waste of cycles anyway...
	 */
	state->result_tape = state->tp_tapenum[state->tapeRange];
	LogicalTapeFreeze(state->tapeset, state->result_tape);
	state->status = TSS_SORTEDONTAPE;
}


/*
 * Merge one run from each input tape, except ones with dummy runs.
 *
 * This is the inner loop of Algorithm D step D5.  We know that the
 * output tape is TAPE[T].
 */
static void
mergeonerun(Tuplesortstate *state)
{
	int			destTape = state->tp_tapenum[state->tapeRange];
	int			srcTape;
	int			tupIndex;
	SortTuple  *tup;
	int64		priorAvail,
				spaceFreed;

	/*
	 * Start the merge by loading one tuple from each active source tape into
	 * the heap.  We can also decrease the input run/dummy run counts.
	 */
	beginmerge(state);

	/*
	 * Execute merge by repeatedly extracting lowest tuple in heap, writing it
	 * out, and replacing it with next tuple from same tape (if there is
	 * another one).
	 */
	while (state->memtupcount > 0)
	{
		bool checkIndex = true;	/* for HEAPCOMPARE */
		if (state-> dedup && state->memtupcount > 1 &&
			HEAPCOMPARE(&state->memtuples[0], &state->memtuples[1]) == 0)
		{				/* A dup; drop one */
			FREEMEM(state, GetMemoryChunkSpace(state->memtuples[0].tuple));
			heap_free_minimal_tuple(state->memtuples[0].tuple);
		}
		else
		{
			/* write the tuple to destTape */
			priorAvail = state->availMem;
			srcTape = state->memtuples[0].tupindex;
			WRITETUP(state, destTape, &state->memtuples[0]);
			/* writetup adjusted total free space, now fix per-tape space */
			spaceFreed = state->availMem - priorAvail;
			state->mergeavailmem[srcTape] += spaceFreed;
		}
		/* compact the heap */
		tuplesort_heap_siftup(state, false);
		if ((tupIndex = state->mergenext[srcTape]) == 0)
		{
			/* out of preloaded data on this tape, try to read more */
			mergepreread(state);
			/* if still no data, we've reached end of run on this tape */
			if ((tupIndex = state->mergenext[srcTape]) == 0)
				continue;
		}
		/* pull next preread tuple from list, insert in heap */
		tup = &state->memtuples[tupIndex];
		state->mergenext[srcTape] = tup->tupindex;
		if (state->mergenext[srcTape] == 0)
			state->mergelast[srcTape] = 0;
		tuplesort_heap_insert(state, tup, srcTape, false);
		/* put the now-unused memtuples entry on the freelist */
		tup->tupindex = state->mergefreelist;
		state->mergefreelist = tupIndex;
		state->mergeavailslots[srcTape]++;
	}

	/*
	 * When the heap empties, we're done.  Write an end-of-run marker on the
	 * output tape, and increment its count of real runs.
	 */
	markrunend(state, destTape);
	state->tp_runs[state->tapeRange]++;

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG, "finished %d-way merge step: %s", state->activeTapes,
			 pg_rusage_show(&state->ru_start));
#endif
}

/*
 * beginmerge - initialize for a merge pass
 *
 * We decrease the counts of real and dummy runs for each tape, and mark
 * which tapes contain active input runs in mergeactive[].	Then, load
 * as many tuples as we can from each active input tape, and finally
 * fill the merge heap with the first tuple from each active tape.
 */
static void
beginmerge(Tuplesortstate *state)
{
	int			activeTapes;
	int			tapenum;
	int			srcTape;
	int			slotsPerTape;
	int64		spacePerTape;

	/* Heap should be empty here */
	Assert(state->memtupcount == 0);

	/* Adjust run counts and mark the active tapes */
	memset(state->mergeactive, 0,
		   state->maxTapes * sizeof(*state->mergeactive));
	activeTapes = 0;
	for (tapenum = 0; tapenum < state->tapeRange; tapenum++)
	{
		if (state->tp_dummy[tapenum] > 0)
			state->tp_dummy[tapenum]--;
		else
		{
			Assert(state->tp_runs[tapenum] > 0);
			state->tp_runs[tapenum]--;
			srcTape = state->tp_tapenum[tapenum];
			state->mergeactive[srcTape] = true;
			activeTapes++;
		}
	}
	state->activeTapes = activeTapes;

	/* Clear merge-pass state variables */
	memset(state->mergenext, 0,
		   state->maxTapes * sizeof(*state->mergenext));
	memset(state->mergelast, 0,
		   state->maxTapes * sizeof(*state->mergelast));
	state->mergefreelist = 0;	/* nothing in the freelist */
	state->mergefirstfree = activeTapes;		/* 1st slot avail for preread */

	/*
	 * Initialize space allocation to let each active input tape have an equal
	 * share of preread space.
	 */
	Assert(activeTapes > 0);
	slotsPerTape = (state->memtupsize - state->mergefirstfree) / activeTapes;
	Assert(slotsPerTape > 0);
	spacePerTape = state->availMem / activeTapes;
	for (srcTape = 0; srcTape < state->maxTapes; srcTape++)
	{
		if (state->mergeactive[srcTape])
		{
			state->mergeavailslots[srcTape] = slotsPerTape;
			state->mergeavailmem[srcTape] = spacePerTape;
		}
	}

	/*
	 * Preread as many tuples as possible (and at least one) from each active
	 * tape
	 */
	mergepreread(state);

	/* Load the merge heap with the first tuple from each input tape */
	for (srcTape = 0; srcTape < state->maxTapes; srcTape++)
	{
		int			tupIndex = state->mergenext[srcTape];
		SortTuple  *tup;

		if (tupIndex)
		{
			tup = &state->memtuples[tupIndex];
			state->mergenext[srcTape] = tup->tupindex;
			if (state->mergenext[srcTape] == 0)
				state->mergelast[srcTape] = 0;
			tuplesort_heap_insert(state, tup, srcTape, false);
			/* put the now-unused memtuples entry on the freelist */
			tup->tupindex = state->mergefreelist;
			state->mergefreelist = tupIndex;
			state->mergeavailslots[srcTape]++;
		}
	}
}

/*
 * mergepreread - load tuples from merge input tapes
 *
 * This routine exists to improve sequentiality of reads during a merge pass,
 * as explained in the header comments of this file.  Load tuples from each
 * active source tape until the tape's run is exhausted or it has used up
 * its fair share of available memory.	In any case, we guarantee that there
 * is at least one preread tuple available from each unexhausted input tape.
 *
 * We invoke this routine at the start of a merge pass for initial load,
 * and then whenever any tape's preread data runs out.  Note that we load
 * as much data as possible from all tapes, not just the one that ran out.
 * This is because logtape.c works best with a usage pattern that alternates
 * between reading a lot of data and writing a lot of data, so whenever we
 * are forced to read, we should fill working memory completely.
 *
 * In FINALMERGE state, we *don't* use this routine, but instead just preread
 * from the single tape that ran dry.  There's no read/write alternation in
 * that state and so no point in scanning through all the tapes to fix one.
 * (Moreover, there may be quite a lot of inactive tapes in that state, since
 * we might have had many fewer runs than tapes.  In a regular tape-to-tape
 * merge we can expect most of the tapes to be active.)
 */
static void
mergepreread(Tuplesortstate *state)
{
	int			srcTape;

	for (srcTape = 0; srcTape < state->maxTapes; srcTape++)
		mergeprereadone(state, srcTape);
}

/*
 * mergeprereadone - load tuples from one merge input tape
 *
 * Read tuples from the specified tape until it has used up its free memory
 * or array slots; but ensure that we have at least one tuple, if any are
 * to be had.
 */
static void
mergeprereadone(Tuplesortstate *state, int srcTape)
{
	unsigned int tuplen;
	SortTuple	stup;
	int			tupIndex;
	int64		priorAvail,
				spaceUsed;

	if (!state->mergeactive[srcTape])
		return;					/* tape's run is already exhausted */
	priorAvail = state->availMem;
	state->availMem = state->mergeavailmem[srcTape];
	while ((state->mergeavailslots[srcTape] > 0 && !LACKMEM(state)) ||
		   state->mergenext[srcTape] == 0)
	{
		/* read next tuple, if any */
		if ((tuplen = getlen(state, srcTape, true)) == 0)
		{
			state->mergeactive[srcTape] = false;
			break;
		}
		READTUP(state, &stup, srcTape, tuplen);
		/* find a free slot in memtuples[] for it */
		tupIndex = state->mergefreelist;
		if (tupIndex)
			state->mergefreelist = state->memtuples[tupIndex].tupindex;
		else
		{
			tupIndex = state->mergefirstfree++;
			Assert(tupIndex < state->memtupsize);
		}
		state->mergeavailslots[srcTape]--;
		/* store tuple, append to list for its tape */
		stup.tupindex = 0;
		state->memtuples[tupIndex] = stup;
		if (state->mergelast[srcTape])
			state->memtuples[state->mergelast[srcTape]].tupindex = tupIndex;
		else
			state->mergenext[srcTape] = tupIndex;
		state->mergelast[srcTape] = tupIndex;
	}
	/* update per-tape and global availmem counts */
	spaceUsed = state->mergeavailmem[srcTape] - state->availMem;
	state->mergeavailmem[srcTape] = state->availMem;
	state->availMem = priorAvail - spaceUsed;
}


/*
 * dumptuples - remove tuples from heap and write to tape
 *
 * This is used during initial-run building, but not during merging.
 *
 * When alltuples = false, dump only enough tuples to get under the
 * availMem limit (and leave at least one tuple in the heap in any case,
 * since puttuple assumes it always has a tuple to compare to).  We also
 * insist there be at least one free slot in the memtuples[] array.
 *
 * When alltuples = true, dump everything currently in memory.
 * (This case is only used at end of input data.)
 *
 * If we empty the heap, close out the current run and return (this should
 * only happen at end of input data).  If we see that the tuple run number
 * at the top of the heap has changed, start a new run.
 */
static void
dumptuples(Tuplesortstate *state, bool alltuples)
{
	while (alltuples ||
		   (LACKMEM(state) && state->memtupcount > 1) ||
		   state->memtupcount >= state->memtupsize)
	{
		bool checkIndex = true;	/* for HEAPCOMPARE */
		/*
		 * Dump the heap's frontmost entry, and sift up to remove it from the
		 * heap.
		 */
		Assert(state->memtupcount > 0);

		if (state-> dedup && state->memtupcount > 1 &&
			HEAPCOMPARE(&state->memtuples[0], &state->memtuples[1]) == 0)
		{				/* A dup; drop one */
			FREEMEM(state, GetMemoryChunkSpace(state->memtuples[0].tuple));
			heap_free_minimal_tuple(state->memtuples[0].tuple);
		}
		else
			WRITETUP(state, state->tp_tapenum[state->destTape],
				 &state->memtuples[0]);

		tuplesort_heap_siftup(state, true);

		/*
		 * If the heap is empty *or* top run number has changed, we've
		 * finished the current run.
		 */
		if (state->memtupcount == 0 ||
			state->currentRun != state->memtuples[0].tupindex)
		{
			markrunend(state, state->tp_tapenum[state->destTape]);
			state->currentRun++;
			state->tp_runs[state->destTape]++;
			state->tp_dummy[state->destTape]--; /* per Alg D step D2 */

#ifdef TRACE_SORT
			if (trace_sort)
				elog(LOG, "finished writing%s run %d to tape %d: %s",
					 (state->memtupcount == 0) ? " final" : "",
					 state->currentRun, state->destTape,
					 pg_rusage_show(&state->ru_start));
#endif

			/*
			 * Done if heap is empty, else prepare for new run.
			 */
			if (state->memtupcount == 0)
				break;
			Assert(state->currentRun == state->memtuples[0].tupindex);
			selectnewtape(state);
		}
	}
}

/*
 * tuplesort_rescan		- rewind and replay the scan
 */
void
tuplesort_rescan(Tuplesortstate *state)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

	Assert(state->randomAccess);

	switch (state->status)
	{
		case TSS_SORB:
		case TSS_SORB_ONESHOT:
			state->current = state->markpos_offset = state->list_start;
			state->eof_reached = state->markpos_eof = false;
			break;
		case TSS_SORTEDINMEM:
			state->current = 0;
			state->eof_reached = false;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;
		case TSS_SORTEDONTAPE:
			LogicalTapeRewind(state->tapeset,
							  state->result_tape,
							  false);
			state->eof_reached = false;
			state->markpos_block = 0L;
			state->markpos_offset = 0;
			state->markpos_eof = false;
			break;
		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * tuplesort_markpos	- saves current position in the merged sort file
 */
void
tuplesort_markpos(Tuplesortstate *state)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

	Assert(state->randomAccess);

	switch (state->status)
	{
		case TSS_SORB:
		case TSS_SORB_ONESHOT:
		case TSS_SORTEDINMEM:
			state->markpos_offset = state->current;
			state->markpos_eof = state->eof_reached;
			break;
		case TSS_SORTEDONTAPE:
			LogicalTapeTell(state->tapeset,
							state->result_tape,
							&state->markpos_block,
							&state->markpos_offset);
			state->markpos_eof = state->eof_reached;
			break;
		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * tuplesort_restorepos - restores current position in merged sort file to
 *						  last saved position
 */
void
tuplesort_restorepos(Tuplesortstate *state)
{
	MemoryContext oldcontext = MemoryContextSwitchTo(state->sortcontext);

	Assert(state->randomAccess);

	switch (state->status)
	{
		case TSS_SORB:
		case TSS_SORB_ONESHOT:
		case TSS_SORTEDINMEM:
			state->current = state->markpos_offset;
			state->eof_reached = state->markpos_eof;
			break;
		case TSS_SORTEDONTAPE:
			if (!LogicalTapeSeek(state->tapeset,
								 state->result_tape,
								 state->markpos_block,
								 state->markpos_offset))
				elog(ERROR, "tuplesort_restorepos failed");
			state->eof_reached = state->markpos_eof;
			break;
		default:
			elog(ERROR, "invalid tuplesort state");
			break;
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * tuplesort_get_stats - extract summary statistics
 *
 * This can be called after tuplesort_performsort() finishes to obtain
 * printable summary information about how the sort was performed.
 * spaceUsed is measured in kilobytes.
 */
void
tuplesort_get_stats(Tuplesortstate *state,
					const char **sortMethod,
					const char **spaceType,
					long *spaceUsed)
{
	/*
	 * Note: it might seem we should provide both memory and disk usage for a
	 * disk-based sort.  However, the current code doesn't track memory space
	 * accurately once we have begun to return tuples to the caller (since we
	 * don't account for pfree's the caller is expected to do), so we cannot
	 * rely on availMem in a disk sort.  This does not seem worth the overhead
	 * to fix.	Is it worth creating an API for the memory context code to
	 * tell us how much is actually used in sortcontext?
	 */
	if (state->tapeset)
	{
		*spaceType = "Disk";
		*spaceUsed = LogicalTapeSetBlocks(state->tapeset) * (BLCKSZ / 1024);
	}
	else
	{
		*spaceType = "Memory";
		*spaceUsed = (state->allowedMem - state->availMem + 1023) / 1024;
	}

	switch (state->status)
	{
		case TSS_SORB:
		case TSS_SORB_ONESHOT:
			*sortMethod = state->boundUsed
				? state->dedup
					? "first-N dedup internal merge"
					: "first-N internal merge"
				: state->dedup
					? "dedup internal merge"
					: "internal merge";
			break;
		case TSS_SORTEDINMEM:
			if (state->boundUsed)
				*sortMethod = "top-N heapsort";
			else
				*sortMethod = "quicksort";
			break;
		case TSS_SORTEDONTAPE:
			*sortMethod = state->dedup
				?  "dedup external sort"
				:  "external sort";
			break;
		case TSS_FINALMERGE:
			*sortMethod = state->dedup
				?  "dedup external merge"
				:  "external merge";
			break;
		default:
			*sortMethod = "still in progress";
			break;
	}
}


/*
 * Heap manipulation routines, per Knuth's Algorithm 5.2.3H.
 */

/*
 * Convert the existing unordered array of SortTuples to a bounded heap,
 * discarding all but the smallest "state->bound" tuples.
 *
 * When working with a bounded heap, we want to keep the largest entry
 * at the root (array entry zero), instead of the smallest as in the normal
 * sort case.  This allows us to discard the largest entry cheaply.
 * Therefore, we temporarily reverse the sort direction.
 *
 * We assume that all entries in a bounded heap will always have tupindex
 * zero; it therefore doesn't matter that HEAPCOMPARE() doesn't reverse
 * the direction of comparison for tupindexes.
 */
static void
make_bounded_heap(Tuplesortstate *state)
{
	int			tupcount = state->memtupcount;
	int			i;

	Assert(state->status == TSS_INITIAL);
	Assert(state->bounded);
	Assert(tupcount >= state->bound);

	/* Reverse sort direction so largest entry will be at root */
	REVERSEDIRECTION(state);

	state->memtupcount = 0;		/* make the heap empty */
	for (i = 0; i < tupcount; i++)
	{
		if (state->memtupcount >= state->bound &&
		  COMPARETUP(state, &state->memtuples[i], &state->memtuples[0]) <= 0)
		{
			/* New tuple would just get thrown out, so skip it */
			free_sort_tuple(state, &state->memtuples[i]);
			CHECK_FOR_INTERRUPTS();
		}
		else
		{
			/* Insert next tuple into heap */
			/* Must copy source tuple to avoid possible overwrite */
			SortTuple	stup = state->memtuples[i];

			tuplesort_heap_insert(state, &stup, 0, false);

			/* If heap too full, discard largest entry */
			if (state->memtupcount > state->bound)
			{
				free_sort_tuple(state, &state->memtuples[0]);
				tuplesort_heap_siftup(state, false);
			}
		}
	}

	Assert(state->memtupcount == state->bound);
	state->status = TSS_BOUNDED;
}

/*
 * Convert the bounded heap to a properly-sorted array
 */
static void
sort_bounded_heap(Tuplesortstate *state)
{
	int			tupcount = state->memtupcount;

	Assert(state->status == TSS_BOUNDED);
	Assert(state->bounded);
	Assert(tupcount == state->bound);

	/*
	 * We can unheapify in place because each sift-up will remove the largest
	 * entry, which we can promptly store in the newly freed slot at the end.
	 * Once we're down to a single-entry heap, we're done.
	 */
	while (state->memtupcount > 1)
	{
		SortTuple	stup = state->memtuples[0];

		/* this sifts-up the next-largest entry and decreases memtupcount */
		tuplesort_heap_siftup(state, false);
		state->memtuples[state->memtupcount] = stup;
	}
	state->memtupcount = tupcount;

	/*
	 * Reverse sort direction back to the original state.  This is not
	 * actually necessary but seems like a good idea for tidiness.
	 */
	REVERSEDIRECTION(state);

	state->status = TSS_SORTEDINMEM;
	state->boundUsed = true;
}

/*
 * Insert a new tuple into an empty or existing heap, maintaining the
 * heap invariant.	Caller is responsible for ensuring there's room.
 *
 * Note: we assume *tuple is a temporary variable that can be scribbled on.
 * For some callers, tuple actually points to a memtuples[] entry above the
 * end of the heap.  This is safe as long as it's not immediately adjacent
 * to the end of the heap (ie, in the [memtupcount] array entry) --- if it
 * is, it might get overwritten before being moved into the heap!
 */
static void
tuplesort_heap_insert(Tuplesortstate *state, SortTuple *tuple,
					  int tupleindex, bool checkIndex)
{
	SortTuple  *memtuples;
	int			j;

	/*
	 * Save the tupleindex --- see notes above about writing on *tuple. It's a
	 * historical artifact that tupleindex is passed as a separate argument
	 * and not in *tuple, but it's notationally convenient so let's leave it
	 * that way.
	 */
	tuple->tupindex = tupleindex;

	memtuples = state->memtuples;
	Assert(state->memtupcount < state->memtupsize);

	CHECK_FOR_INTERRUPTS();

	/*
	 * Sift-up the new entry, per Knuth 5.2.3 exercise 16. Note that Knuth is
	 * using 1-based array indexes, not 0-based.
	 */
	j = state->memtupcount++;
	while (j > 0)
	{
		int			i = (j - 1) >> 1;

		if (HEAPCOMPARE(tuple, &memtuples[i]) >= 0)
			break;
		memtuples[j] = memtuples[i];
		j = i;
	}
	memtuples[j] = *tuple;
}

/*
 * The tuple at state->memtuples[0] has been removed from the heap.
 * Decrement memtupcount, and sift up to maintain the heap invariant.
 */
static void
tuplesort_heap_siftup(Tuplesortstate *state, bool checkIndex)
{
	SortTuple  *memtuples = state->memtuples;
	SortTuple  *tuple;
	int			i,
				n;

	if (--state->memtupcount <= 0)
		return;

	CHECK_FOR_INTERRUPTS();

	n = state->memtupcount;
	tuple = &memtuples[n];		/* tuple that must be reinserted */
	i = 0;						/* i is where the "hole" is */
	for (;;)
	{
		int			j = 2 * i + 1;

		if (j >= n)
			break;
		if (j + 1 < n &&
			HEAPCOMPARE(&memtuples[j], &memtuples[j + 1]) > 0)
			j++;
		if (HEAPCOMPARE(tuple, &memtuples[j]) <= 0)
			break;
		memtuples[i] = memtuples[j];
		i = j;
	}
	memtuples[i] = *tuple;
}


/*
 * Tape interface routines
 */

static unsigned int
getlen(Tuplesortstate *state, int tapenum, bool eofOK)
{
	unsigned int len;

	if (LogicalTapeRead(state->tapeset, tapenum,
						&len, sizeof(len)) != sizeof(len))
		elog(ERROR, "unexpected end of tape");
	if (len == 0 && !eofOK)
		elog(ERROR, "unexpected end of data");
	return len;
}

static void
markrunend(Tuplesortstate *state, int tapenum)
{
	unsigned int len = 0;

	LogicalTapeWrite(state->tapeset, tapenum, (void *) &len, sizeof(len));
}


/*
 * Inline-able copy of FunctionCall2Coll() to save some cycles in sorting.
 */
static inline Datum
myFunctionCall2Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2)
{
	FunctionCallInfoData fcinfo;
	Datum		result;

	InitFunctionCallInfoData(fcinfo, flinfo, 2, collation, NULL, NULL);

	fcinfo.arg[0] = arg1;
	fcinfo.arg[1] = arg2;
	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;

	result = FunctionCallInvoke(&fcinfo);

	/* Check for null result, since caller is clearly not expecting one */
	if (fcinfo.isnull)
		elog(ERROR, "function %u returned NULL", fcinfo.flinfo->fn_oid);

	return result;
}

/*
 * Apply a sort function (by now converted to fmgr lookup form)
 * and return a 3-way comparison result.  This takes care of handling
 * reverse-sort and NULLs-ordering properly.  We assume that DESC and
 * NULLS_FIRST options are encoded in sk_flags the same way btree does it.
 */
static inline int32
inlineApplySortFunction(FmgrInfo *sortFunction, int sk_flags, Oid collation,
						Datum datum1, bool isNull1,
						Datum datum2, bool isNull2)
{
	int32		compare;

	if (isNull1)
	{
		if (isNull2)
			compare = 0;		/* NULL "=" NULL */
		else if (sk_flags & SK_BT_NULLS_FIRST)
			compare = -1;		/* NULL "<" NOT_NULL */
		else
			compare = 1;		/* NULL ">" NOT_NULL */
	}
	else if (isNull2)
	{
		if (sk_flags & SK_BT_NULLS_FIRST)
			compare = 1;		/* NOT_NULL ">" NULL */
		else
			compare = -1;		/* NOT_NULL "<" NULL */
	}
	else
	{
		compare = DatumGetInt32(myFunctionCall2Coll(sortFunction, collation,
													datum1, datum2));

		if (sk_flags & SK_BT_DESC)
			compare = -compare;
	}

	return compare;
}


/*
 * Routines specialized for HeapTuple (actually MinimalTuple) case
 */

static int
comparetup_heap(const SortTuple *a, const SortTuple *b, Tuplesortstate *state)
{
	SortSupport sortKey = state->sortKeys;
	HeapTupleData ltup;
	HeapTupleData rtup;
	TupleDesc	tupDesc;
	int			nkey;
	int32		compare;

	/* Compare the leading sort key */
	compare = ApplySortComparator(a->datum1, a->isnull1,
								  b->datum1, b->isnull1,
								  sortKey);
	if (compare != 0)
		return compare;

	/* Compare additional sort keys */
	ltup.t_len = ((MinimalTuple) a->tuple)->t_len + MINIMAL_TUPLE_OFFSET;
	ltup.t_data = (HeapTupleHeader) ((char *) a->tuple - MINIMAL_TUPLE_OFFSET);
	rtup.t_len = ((MinimalTuple) b->tuple)->t_len + MINIMAL_TUPLE_OFFSET;
	rtup.t_data = (HeapTupleHeader) ((char *) b->tuple - MINIMAL_TUPLE_OFFSET);
	tupDesc = state->tupDesc;
	sortKey++;
	for (nkey = 1; nkey < state->nKeys; nkey++, sortKey++)
	{
		AttrNumber	attno = sortKey->ssup_attno;
		Datum		datum1,
					datum2;
		bool		isnull1,
					isnull2;

		datum1 = heap_getattr(&ltup, attno, tupDesc, &isnull1);
		datum2 = heap_getattr(&rtup, attno, tupDesc, &isnull2);

		compare = ApplySortComparator(datum1, isnull1,
									  datum2, isnull2,
									  sortKey);
		if (compare != 0)
			return compare;
	}

	return 0;
}

static void
copytup_heap(Tuplesortstate *state, SortTuple *stup, void *tup)
{
	/*
	 * We expect the passed "tup" to be a TupleTableSlot, and form a
	 * MinimalTuple using the exported interface for that.
	 */
	TupleTableSlot *slot = (TupleTableSlot *) tup;
	MinimalTuple tuple;
	HeapTupleData htup;
	bool isnull1;

	/* copy the tuple into sort storage */
	tuple = ExecCopySlotMinimalTuple(slot);
	stup->tuple = (void *) tuple;
	USEMEM(state, GetMemoryChunkSpace(tuple));
	/* set up first-column key value */
	htup.t_len = tuple->t_len + MINIMAL_TUPLE_OFFSET;
	htup.t_data = (HeapTupleHeader) ((char *) tuple - MINIMAL_TUPLE_OFFSET);
	stup->datum1 = heap_getattr(&htup,
								state->sortKeys[0].ssup_attno,
								state->tupDesc,
								&isnull1);
	stup->isnull1 = isnull1;
}

static void
writetup_heap(Tuplesortstate *state, int tapenum, SortTuple *stup)
{
	MinimalTuple tuple = (MinimalTuple) stup->tuple;

	/* the part of the MinimalTuple we'll write: */
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;
	unsigned int tupbodylen = tuple->t_len - MINIMAL_TUPLE_DATA_OFFSET;

	/* total on-disk footprint: */
	unsigned int tuplen = tupbodylen + sizeof(int);

	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) &tuplen, sizeof(tuplen));
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) tupbody, tupbodylen);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeWrite(state->tapeset, tapenum,
						 (void *) &tuplen, sizeof(tuplen));

	FREEMEM(state, GetMemoryChunkSpace(tuple));
	heap_free_minimal_tuple(tuple);
}

static void
readtup_heap(Tuplesortstate *state, SortTuple *stup,
			 int tapenum, unsigned int len)
{
	unsigned int tupbodylen = len - sizeof(int);
	unsigned int tuplen = tupbodylen + MINIMAL_TUPLE_DATA_OFFSET;
	MinimalTuple tuple = (MinimalTuple) palloc(tuplen);
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;
	HeapTupleData htup;
	bool isnull1;

	USEMEM(state, GetMemoryChunkSpace(tuple));
	/* read in the tuple proper */
	tuple->t_len = tuplen;
	LogicalTapeReadExact(state->tapeset, tapenum,
						 tupbody, tupbodylen);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeReadExact(state->tapeset, tapenum,
							 &tuplen, sizeof(tuplen));
	stup->tuple = (void *) tuple;
	/* set up first-column key value */
	htup.t_len = tuple->t_len + MINIMAL_TUPLE_OFFSET;
	htup.t_data = (HeapTupleHeader) ((char *) tuple - MINIMAL_TUPLE_OFFSET);
	stup->datum1 = heap_getattr(&htup,
								state->sortKeys[0].ssup_attno,
								state->tupDesc,
								&isnull1);
	stup->isnull1 = isnull1;
}

static void
reversedirection_heap(Tuplesortstate *state)
{
	SortSupport sortKey = state->sortKeys;
	int			nkey;

	for (nkey = 0; nkey < state->nKeys; nkey++, sortKey++)
	{
		sortKey->ssup_reverse = !sortKey->ssup_reverse;
		sortKey->ssup_nulls_first = !sortKey->ssup_nulls_first;
	}
}


/*
 * Routines specialized for the CLUSTER case (HeapTuple data, with
 * comparisons per a btree index definition)
 */

static int
comparetup_cluster(const SortTuple *a, const SortTuple *b,
				   Tuplesortstate *state)
{
	ScanKey		scanKey = state->indexScanKey;
	HeapTuple	ltup;
	HeapTuple	rtup;
	TupleDesc	tupDesc;
	int			nkey;
	int32		compare;

	/* Compare the leading sort key, if it's simple */
	if (state->indexInfo->ii_KeyAttrNumbers[0] != 0)
	{
		compare = inlineApplySortFunction(&scanKey->sk_func, scanKey->sk_flags,
										  scanKey->sk_collation,
										  a->datum1, a->isnull1,
										  b->datum1, b->isnull1);
		if (compare != 0 || state->nKeys == 1)
			return compare;
		/* Compare additional columns the hard way */
		scanKey++;
		nkey = 1;
	}
	else
	{
		/* Must compare all keys the hard way */
		nkey = 0;
	}

	/* Compare additional sort keys */
	ltup = (HeapTuple) a->tuple;
	rtup = (HeapTuple) b->tuple;

	if (state->indexInfo->ii_Expressions == NULL)
	{
		/* If not expression index, just compare the proper heap attrs */
		tupDesc = state->tupDesc;

		for (; nkey < state->nKeys; nkey++, scanKey++)
		{
			AttrNumber	attno = state->indexInfo->ii_KeyAttrNumbers[nkey];
			Datum		datum1,
						datum2;
			bool		isnull1,
						isnull2;

			datum1 = heap_getattr(ltup, attno, tupDesc, &isnull1);
			datum2 = heap_getattr(rtup, attno, tupDesc, &isnull2);

			compare = inlineApplySortFunction(&scanKey->sk_func,
											  scanKey->sk_flags,
											  scanKey->sk_collation,
											  datum1, isnull1,
											  datum2, isnull2);
			if (compare != 0)
				return compare;
		}
	}
	else
	{
		/*
		 * In the expression index case, compute the whole index tuple and
		 * then compare values.  It would perhaps be faster to compute only as
		 * many columns as we need to compare, but that would require
		 * duplicating all the logic in FormIndexDatum.
		 */
		Datum		l_index_values[INDEX_MAX_KEYS];
		bool		l_index_isnull[INDEX_MAX_KEYS];
		Datum		r_index_values[INDEX_MAX_KEYS];
		bool		r_index_isnull[INDEX_MAX_KEYS];
		TupleTableSlot *ecxt_scantuple;

		/* Reset context each time to prevent memory leakage */
		ResetPerTupleExprContext(state->estate);

		ecxt_scantuple = GetPerTupleExprContext(state->estate)->ecxt_scantuple;

		ExecStoreTuple(ltup, ecxt_scantuple, InvalidBuffer, false);
		FormIndexDatum(state->indexInfo, ecxt_scantuple, state->estate,
					   l_index_values, l_index_isnull);

		ExecStoreTuple(rtup, ecxt_scantuple, InvalidBuffer, false);
		FormIndexDatum(state->indexInfo, ecxt_scantuple, state->estate,
					   r_index_values, r_index_isnull);

		for (; nkey < state->nKeys; nkey++, scanKey++)
		{
			compare = inlineApplySortFunction(&scanKey->sk_func,
											  scanKey->sk_flags,
											  scanKey->sk_collation,
											  l_index_values[nkey],
											  l_index_isnull[nkey],
											  r_index_values[nkey],
											  r_index_isnull[nkey]);
			if (compare != 0)
				return compare;
		}
	}

	return 0;
}

static void
copytup_cluster(Tuplesortstate *state, SortTuple *stup, void *tup)
{
	HeapTuple	tuple = (HeapTuple) tup;
	bool isnull1;

	/* copy the tuple into sort storage */
	tuple = heap_copytuple(tuple);
	stup->tuple = (void *) tuple;
	USEMEM(state, GetMemoryChunkSpace(tuple));
	/* set up first-column key value, if it's a simple column */
	if (state->indexInfo->ii_KeyAttrNumbers[0] != 0)
		stup->datum1 = heap_getattr(tuple,
									state->indexInfo->ii_KeyAttrNumbers[0],
									state->tupDesc,
									&isnull1);
	stup->isnull1 = isnull1;
}

static void
writetup_cluster(Tuplesortstate *state, int tapenum, SortTuple *stup)
{
	HeapTuple	tuple = (HeapTuple) stup->tuple;
	unsigned int tuplen = tuple->t_len + sizeof(ItemPointerData) + sizeof(int);

	/* We need to store t_self, but not other fields of HeapTupleData */
	LogicalTapeWrite(state->tapeset, tapenum,
					 &tuplen, sizeof(tuplen));
	LogicalTapeWrite(state->tapeset, tapenum,
					 &tuple->t_self, sizeof(ItemPointerData));
	LogicalTapeWrite(state->tapeset, tapenum,
					 tuple->t_data, tuple->t_len);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeWrite(state->tapeset, tapenum,
						 &tuplen, sizeof(tuplen));

	FREEMEM(state, GetMemoryChunkSpace(tuple));
	heap_freetuple(tuple);
}

static void
readtup_cluster(Tuplesortstate *state, SortTuple *stup,
				int tapenum, unsigned int tuplen)
{
	unsigned int t_len = tuplen - sizeof(ItemPointerData) - sizeof(int);
	HeapTuple	tuple = (HeapTuple) palloc(t_len + HEAPTUPLESIZE);
	bool isnull1;

	USEMEM(state, GetMemoryChunkSpace(tuple));
	/* Reconstruct the HeapTupleData header */
	tuple->t_data = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);
	tuple->t_len = t_len;
	LogicalTapeReadExact(state->tapeset, tapenum,
						 &tuple->t_self, sizeof(ItemPointerData));
	/* We don't currently bother to reconstruct t_tableOid */
	tuple->t_tableOid = InvalidOid;
	/* Read in the tuple body */
	LogicalTapeReadExact(state->tapeset, tapenum,
						 tuple->t_data, tuple->t_len);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeReadExact(state->tapeset, tapenum,
							 &tuplen, sizeof(tuplen));
	stup->tuple = (void *) tuple;
	/* set up first-column key value, if it's a simple column */
	if (state->indexInfo->ii_KeyAttrNumbers[0] != 0)
		stup->datum1 = heap_getattr(tuple,
									state->indexInfo->ii_KeyAttrNumbers[0],
									state->tupDesc,
									&isnull1);
	stup->isnull1 = isnull1;
}


/*
 * Routines specialized for IndexTuple case
 *
 * The btree and hash cases require separate comparison functions, but the
 * IndexTuple representation is the same so the copy/write/read support
 * functions can be shared.
 */

static int
comparetup_index_btree(const SortTuple *a, const SortTuple *b,
					   Tuplesortstate *state)
{
	/*
	 * This is similar to _bt_tuplecompare(), but we have already done the
	 * index_getattr calls for the first column, and we need to keep track of
	 * whether any null fields are present.  Also see the special treatment
	 * for equal keys at the end.
	 */
	ScanKey		scanKey = state->indexScanKey;
	IndexTuple	tuple1;
	IndexTuple	tuple2;
	int			keysz;
	TupleDesc	tupDes;
	bool		equal_hasnull = false;
	int			nkey;
	int32		compare;

	/* Compare the leading sort key */
	compare = inlineApplySortFunction(&scanKey->sk_func, scanKey->sk_flags,
									  scanKey->sk_collation,
									  a->datum1, a->isnull1,
									  b->datum1, b->isnull1);
	if (compare != 0)
		return compare;

	/* they are equal, so we only need to examine one null flag */
	if (a->isnull1)
		equal_hasnull = true;

	/* Compare additional sort keys */
	tuple1 = (IndexTuple) a->tuple;
	tuple2 = (IndexTuple) b->tuple;
	keysz = state->nKeys;
	tupDes = RelationGetDescr(state->indexRel);
	scanKey++;
	for (nkey = 2; nkey <= keysz; nkey++, scanKey++)
	{
		Datum		datum1,
					datum2;
		bool		isnull1,
					isnull2;

		datum1 = index_getattr(tuple1, nkey, tupDes, &isnull1);
		datum2 = index_getattr(tuple2, nkey, tupDes, &isnull2);

		compare = inlineApplySortFunction(&scanKey->sk_func, scanKey->sk_flags,
										  scanKey->sk_collation,
										  datum1, isnull1,
										  datum2, isnull2);
		if (compare != 0)
			return compare;		/* done when we find unequal attributes */

		/* they are equal, so we only need to examine one null flag */
		if (isnull1)
			equal_hasnull = true;
	}

	/*
	 * If btree has asked us to enforce uniqueness, complain if two equal
	 * tuples are detected (unless there was at least one NULL field).
	 *
	 * It is sufficient to make the test here, because if two tuples are equal
	 * they *must* get compared at some stage of the sort --- otherwise the
	 * sort algorithm wouldn't have checked whether one must appear before the
	 * other.
	 */
	if (state->enforceUnique && !equal_hasnull)
	{
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];

		/*
		 * Some rather brain-dead implementations of qsort (such as the one in
		 * QNX 4) will sometimes call the comparison routine to compare a
		 * value to itself, but we always use our own implementation, which
		 * does not.
		 */
		Assert(tuple1 != tuple2);

		index_deform_tuple(tuple1, tupDes, values, isnull);
		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("could not create unique index \"%s\"",
						RelationGetRelationName(state->indexRel)),
				 errdetail("Key %s is duplicated.",
						   BuildIndexValueDescription(state->indexRel,
													  values, isnull)),
				 errtableconstraint(state->heapRel,
								 RelationGetRelationName(state->indexRel))));
	}

	/*
	 * If key values are equal, we sort on ItemPointer.  This does not affect
	 * validity of the finished index, but it may be useful to have index
	 * scans in physical order.
	 */
	{
		BlockNumber blk1 = ItemPointerGetBlockNumber(&tuple1->t_tid);
		BlockNumber blk2 = ItemPointerGetBlockNumber(&tuple2->t_tid);

		if (blk1 != blk2)
			return (blk1 < blk2) ? -1 : 1;
	}
	{
		OffsetNumber pos1 = ItemPointerGetOffsetNumber(&tuple1->t_tid);
		OffsetNumber pos2 = ItemPointerGetOffsetNumber(&tuple2->t_tid);

		if (pos1 != pos2)
			return (pos1 < pos2) ? -1 : 1;
	}

	return 0;
}

static int
comparetup_index_hash(const SortTuple *a, const SortTuple *b,
					  Tuplesortstate *state)
{
	uint32		hash1;
	uint32		hash2;
	IndexTuple	tuple1;
	IndexTuple	tuple2;

	/*
	 * Fetch hash keys and mask off bits we don't want to sort by. We know
	 * that the first column of the index tuple is the hash key.
	 */
	Assert(!a->isnull1);
	hash1 = DatumGetUInt32(a->datum1) & state->hash_mask;
	Assert(!b->isnull1);
	hash2 = DatumGetUInt32(b->datum1) & state->hash_mask;

	if (hash1 > hash2)
		return 1;
	else if (hash1 < hash2)
		return -1;

	/*
	 * If hash values are equal, we sort on ItemPointer.  This does not affect
	 * validity of the finished index, but it may be useful to have index
	 * scans in physical order.
	 */
	tuple1 = (IndexTuple) a->tuple;
	tuple2 = (IndexTuple) b->tuple;

	{
		BlockNumber blk1 = ItemPointerGetBlockNumber(&tuple1->t_tid);
		BlockNumber blk2 = ItemPointerGetBlockNumber(&tuple2->t_tid);

		if (blk1 != blk2)
			return (blk1 < blk2) ? -1 : 1;
	}
	{
		OffsetNumber pos1 = ItemPointerGetOffsetNumber(&tuple1->t_tid);
		OffsetNumber pos2 = ItemPointerGetOffsetNumber(&tuple2->t_tid);

		if (pos1 != pos2)
			return (pos1 < pos2) ? -1 : 1;
	}

	return 0;
}

static void
copytup_index(Tuplesortstate *state, SortTuple *stup, void *tup)
{
	IndexTuple	tuple = (IndexTuple) tup;
	unsigned int tuplen = IndexTupleSize(tuple);
	IndexTuple	newtuple;
	bool isnull1;

	/* copy the tuple into sort storage */
	newtuple = (IndexTuple) palloc(tuplen);
	memcpy(newtuple, tuple, tuplen);
	USEMEM(state, GetMemoryChunkSpace(newtuple));
	stup->tuple = (void *) newtuple;
	/* set up first-column key value */
	stup->datum1 = index_getattr(newtuple,
								 1,
								 RelationGetDescr(state->indexRel),
								 &isnull1);
	stup->isnull1 = isnull1;
}

static void
writetup_index(Tuplesortstate *state, int tapenum, SortTuple *stup)
{
	IndexTuple	tuple = (IndexTuple) stup->tuple;
	unsigned int tuplen;

	tuplen = IndexTupleSize(tuple) + sizeof(tuplen);
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) &tuplen, sizeof(tuplen));
	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) tuple, IndexTupleSize(tuple));
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeWrite(state->tapeset, tapenum,
						 (void *) &tuplen, sizeof(tuplen));

	FREEMEM(state, GetMemoryChunkSpace(tuple));
	pfree(tuple);
}

static void
readtup_index(Tuplesortstate *state, SortTuple *stup,
			  int tapenum, unsigned int len)
{
	unsigned int tuplen = len - sizeof(unsigned int);
	IndexTuple	tuple = (IndexTuple) palloc(tuplen);
	bool isnull1;

	USEMEM(state, GetMemoryChunkSpace(tuple));
	LogicalTapeReadExact(state->tapeset, tapenum,
						 tuple, tuplen);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeReadExact(state->tapeset, tapenum,
							 &tuplen, sizeof(tuplen));
	stup->tuple = (void *) tuple;
	/* set up first-column key value */
	stup->datum1 = index_getattr(tuple,
								 1,
								 RelationGetDescr(state->indexRel),
								 &isnull1);
	stup->isnull1 = isnull1;
}

static void
reversedirection_index_btree(Tuplesortstate *state)
{
	ScanKey		scanKey = state->indexScanKey;
	int			nkey;

	for (nkey = 0; nkey < state->nKeys; nkey++, scanKey++)
	{
		scanKey->sk_flags ^= (SK_BT_DESC | SK_BT_NULLS_FIRST);
	}
}

static void
reversedirection_index_hash(Tuplesortstate *state)
{
	/* We don't support reversing direction in a hash index sort */
	elog(ERROR, "reversedirection_index_hash is not implemented");
}


/*
 * Routines specialized for DatumTuple case
 */

static int
comparetup_datum(const SortTuple *a, const SortTuple *b, Tuplesortstate *state)
{
	return ApplySortComparator(a->datum1, a->isnull1,
							   b->datum1, b->isnull1,
							   state->onlyKey);
}

static void
copytup_datum(Tuplesortstate *state, SortTuple *stup, void *tup)
{
	/* Not currently needed */
	elog(ERROR, "copytup_datum() should not be called");
}

static void
writetup_datum(Tuplesortstate *state, int tapenum, SortTuple *stup)
{
	void	   *waddr;
	unsigned int tuplen;
	unsigned int writtenlen;

	if (stup->isnull1)
	{
		waddr = NULL;
		tuplen = 0;
	}
	else if (state->datumTypeByVal)
	{
		waddr = &stup->datum1;
		tuplen = sizeof(Datum);
	}
	else
	{
		waddr = DatumGetPointer(stup->datum1);
		tuplen = datumGetSize(stup->datum1, false, state->datumTypeLen);
		Assert(tuplen != 0);
	}

	writtenlen = tuplen + sizeof(unsigned int);

	LogicalTapeWrite(state->tapeset, tapenum,
					 (void *) &writtenlen, sizeof(writtenlen));
	LogicalTapeWrite(state->tapeset, tapenum,
					 waddr, tuplen);
	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeWrite(state->tapeset, tapenum,
						 (void *) &writtenlen, sizeof(writtenlen));

	if (stup->tuple)
	{
		FREEMEM(state, GetMemoryChunkSpace(stup->tuple));
		pfree(stup->tuple);
	}
}

static void
readtup_datum(Tuplesortstate *state, SortTuple *stup,
			  int tapenum, unsigned int len)
{
	unsigned int tuplen = len - sizeof(unsigned int);

	if (tuplen == 0)
	{
		/* it's NULL */
		stup->datum1 = (Datum) 0;
		stup->isnull1 = true;
		stup->tuple = NULL;
	}
	else if (state->datumTypeByVal)
	{
		Assert(tuplen == sizeof(Datum));
		LogicalTapeReadExact(state->tapeset, tapenum,
							 &stup->datum1, tuplen);
		stup->isnull1 = false;
		stup->tuple = NULL;
	}
	else
	{
		void	   *raddr = palloc(tuplen);

		LogicalTapeReadExact(state->tapeset, tapenum,
							 raddr, tuplen);
		stup->datum1 = PointerGetDatum(raddr);
		stup->isnull1 = false;
		stup->tuple = raddr;
		USEMEM(state, GetMemoryChunkSpace(raddr));
	}

	if (state->randomAccess)	/* need trailing length word? */
		LogicalTapeReadExact(state->tapeset, tapenum,
							 &tuplen, sizeof(tuplen));
}

static void
reversedirection_datum(Tuplesortstate *state)
{
	state->onlyKey->ssup_reverse = !state->onlyKey->ssup_reverse;
	state->onlyKey->ssup_nulls_first = !state->onlyKey->ssup_nulls_first;
}

/*
 * Convenience routine to free a tuple previously loaded into sort memory
 */
static void
free_sort_tuple(Tuplesortstate *state, SortTuple *stup)
{
	FREEMEM(state, GetMemoryChunkSpace(stup->tuple));
	pfree(stup->tuple);
}

/*
 * vi: ts=4 sw=4 ai aw
 */
