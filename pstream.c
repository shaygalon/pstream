#define _GNU_SOURCE
#define _MULTI_THREADED
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/time.h>
#include <unistd.h>
#include <float.h>
#include <string.h>
#include <assert.h>
#include <sched.h>
#include <stdint.h>
#include <inttypes.h>
#ifdef USEHUGE
#include <sys/mman.h>
#endif
#ifdef USENUMA
#include <numa.h>
#endif

/* Pstream version 1.10 - written by Bill Broadley bill@cse.ucdavis.edu

 Designed to expose and quantify parallelism in the memory hierarchy 
 by tracking the performance of varying number of threads over varying
 size arrays of doubles.

 Very loosely based on John D. McCalpin's Stream: 
			http://www.cs.virginia.edu/stream/

 to compile, on solaris or alpha/digital unix add -lrt:
    gcc -D_REENTRANT -Wall -pedantic -O4 pstream.c -lpthread -o pstream
 to run: 
    ./pstream output_file
 to view:
    ./view output_file


 Please send results, and machine config (number and type of cpu's, amount
 and type of ram) to bill@cse.ucdavis.edu
 
 Bugfixes and using conf info instead of consts by Shay Gal-On
 
*/

#define REPEAT 1
#define BENCHMARKS 2
#define MAX_THREADS 1024
#define MAX_ITER 256

struct idThreadParams {
	int id;
	int minThreads;
	int maxThreads;
};

double timeAr[MAX_THREADS][BENCHMARKS * 2];

static int shared_cache = 0;
int minMemory = 500 * 1024 * 1024;
int64_t maxMemory = 2048ULL * 1024ULL * 1024ULL;
double timeStep = 0.25;
int cacheLineSize = 64;		  /* bytes per cacheline */
int64_t cacheSize = 32 * 1024;  /* q6600 = 4MB share per die, or 2MB per core */
double increaseArray = 0.925;
int band = 0;
int lat = 0;
int affinity = 0;
int affinity_wide = 0;
int usenuma = 0;
int pageSize = 4096;
int numPages = 1;
int perCacheLine;
int cacheLinesPerPage;
int cur_threads;
int spread=1;

int64_t maxmem=0, max_cpu=0;

pthread_mutex_t syncera, syncerb, finisher, counter;
pthread_mutexattr_t attrib;
pthread_mutex_t fastmutex = PTHREAD_MUTEX_INITIALIZER;
double begin_time, end_time;
long long scale;

static char *label[4] = { "Add:       ", "Triad:     ",
	"Cleanup;   "
};

double begin;

/* crude wall clock time keeping, returns a double of seconds */
double
second ()
{
	struct timeval tp;
	struct timezone tzp;
	gettimeofday (&tp, &tzp);
	return ((double) tp.tv_sec + (double) tp.tv_usec * 1.e-6);
}

#define lui long unsigned int

int64_t *
align_pointer (int64_t * a1, uint64_t cacheSize, int cacheLineSize, int share,
					int rank)
{
	uint64_t a2;
	int64_t linesInCache;
	int64_t offset;

	a2 = (uint64_t) a1;
	/* align pointer to cache */
	a2 = (a2 + (int64_t) (cacheSize - 1)) & ~(int64_t) (cacheSize - 1);
	if (cacheSize > 0)
	{
		linesInCache = cacheSize/cacheLineSize;
		offset = ((linesInCache*rank)/share) * cacheLineSize;
		a2 = a2 + offset;
	}
	else
	{
		a2 = (uint64_t) a1;
	}
/*	printf ("cacheSize=%d cacheLineSize=%d\n",cacheSize,cacheLineSize); */
/*	printf ("rank=%d share=%d\n",rank,share);
	printf ("linesInCache=%d offset=%d\n",linesInCache,offset);*/
/*	printf ("a2=%" PRIu64 " \n", a2 % cacheSize); */
	return ((int64_t *) a2);
}

void *
sync_thread (int id, char *label)
{
	static int counter = 0;
	static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
	static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

	pthread_mutex_lock (&mutex);
	counter = counter + 1;

	/* for this to be true all threads must be waiting here */
	if (counter < cur_threads)
	{
		pthread_cond_wait (&cond, &mutex);
	}
	else
	{
		pthread_cond_broadcast (&cond);
		counter = 0;
	}
	pthread_mutex_unlock (&mutex);
	return (NULL);
}

void
swap (int64_t * a, int64_t x, int64_t y)
{
	int64_t t;
	t = a[x];
	a[x] = a[y];
	a[y] = t;
}

/* choose between l and h, inclusive of both. */
uint64_t
choose (uint64_t l, uint64_t h)
{
	uint64_t range, smallr, ret;

	range = h - l;
	assert (l <= h);
	smallr = range / perCacheLine;	/* the number of cachelines in
												   the range */
	if (numPages > 0)
	{
		if (smallr > cacheLinesPerPage * numPages)
			smallr = cacheLinesPerPage * numPages;
	}
	/* pick a cache line within the range */
	ret = (l + (uint64_t) (drand48 () * smallr) * perCacheLine);
/*  printf ("l=%lld h=%lld ret=%lld\n",l,h,ret);  */
	assert (ret <= h);
	if (l < h)
		return ret;
	return h;
}



int
logint (int l /* 32-bit word to find the log base 2 of */ )
{
	unsigned int v;				  /* 32-bit word to find the log base 2 of */
	unsigned r = 0;				  /* r will be lg(v) */

	if (l < 0)
	{
		printf ("Sorry, positive only\n");
		exit (-1);
	}
	v = l;
	while (v >>= 1)
	{
		r++;
	}
	return (r);
}

int
follow_ar (int64_t * a, int64_t N, int repeat)
{
	int64_t p = 0;
	int i;
#ifdef CNT
	int cnt;
	cnt = 0;
#endif
	for (i = 0; i < repeat; i++)
	{
		p = a[0];
		while (p > 0)
		{
			p = a[p];
		}
	}
	if (p < 0)
	{
		printf ("oops!\n");
	}

#ifdef CNT
	printf ("cnt=%d\n", cnt);
#endif
	return (a[0]);
}


#ifdef USEAFFINITY
void
set_affinity (struct idThreadParams *id)
{
	cpu_set_t cset;
/*	printf ("id=%d affinity=%d affinity_wide=%d\n",id,affinity,affinity_wide); */
	int aid=id->id;
	// Allow spread at constant intervals.
	if (affinity_wide && spread>1) {
		aid= ((aid%spread) * max_cpu/spread) + aid/spread;
	}
	
	sched_getaffinity (0, sizeof (cpu_set_t), &cset);
	CPU_ZERO (&cset);
	CPU_SET (aid, &cset);
	sched_setaffinity (0, sizeof (cpu_set_t), &cset);
}
#endif

void *
latency_thread (void *arg)
{
	struct idThreadParams *id = arg;
	int64_t *a;
	int64_t *aa = NULL;
	int64_t x, y;
	int64_t i, c;
	int64_t size, len = 0;

#ifdef USEAFFINITY
	if (affinity)
		set_affinity (id);
#endif
	size = maxmem / sizeof (int64_t);
	if (usenuma)
	{
#ifdef USENUMA
		numa_run_on_node (id->id % id->maxThreads);
		aa =
			numa_alloc_local (size * sizeof (int64_t) + 2 * cacheSize +
									2 * cacheLineSize);
#endif
	}
	else
	{
		len = (size * sizeof (uint64_t) + 2 * cacheSize + 2 * cacheLineSize);
#ifdef USEHUGE
		len = (len + 2097151) & ~2097151;
		aa = mmap (0, len, PROT_READ | PROT_WRITE,
					  MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB, -1, 0);
		if (aa == MAP_FAILED)
		{
			printf ("Warning memory allocation of %" PRIu64 " MB array failed\n",
					  len/ (1024 * 1024));
			exit (-1);
		}


#else
		aa = (int64_t *) malloc (len);
#endif
	}
	/* allocate the entire cache */
/*	a = (int64_t *) align_pointer (aa, cacheSize, cacheLineSize, 1, 0); */
	a = aa;
#ifdef DEBUG
	printf ("aa=%p a=%p d=%d align=%d t=%d\n",
			  aa, a, (int64_t) ((int64_t) a - (int64_t) aa),
			  ((int64_t) (a)) % (cacheSize), id.id);
#endif /* debug */

	srand48 ((long int) getpid ());
	for (i = 0; i < size; i = i + perCacheLine)
	{
		a[i] = i + perCacheLine;	/* assign each int the index of the next int */
	}
	a[(i - perCacheLine)] = 0;	/* makes the array a loop */
#if DEBUG
	printAr (a, size);
#endif
	for (i = 0; i < (size - perCacheLine); i = i + perCacheLine)
	{
		c = choose (i, size - perCacheLine);
		if (c > (size - perCacheLine))
		{
			printf ("this should never happen *****************\n");
		}
		x = a[i];
		y = a[c];
		swap (a, i, c);
		swap (a, x, y);
	}
#if DEBUG
	printAr (a, size);
#endif
	sync_thread (id->id, label[0]);
	timeAr[id->id][0] = second ();
	follow_ar (a, size, scale);
	timeAr[id->id][1] = second ();
	sync_thread (id->id, label[1]);
#if DEBUG
	printf ("synced numa=%d\n", usenuma);
#endif
/*	sync_thread (id, label[2]); */
	if (usenuma)
	{
#ifdef USENUMA
		numa_free (aa, size * sizeof (int64_t) + cacheSize);
#endif
	}
	else
	{
#ifdef USEHUGE
		munmap (aa, len);
#else
		free (aa);
#endif
	}
#if DEBUG
	printf ("freed %d\n", id);
#endif
	pthread_exit (NULL);
	return NULL;
}


double bandwidthAr[MAX_THREADS][MAX_ITER][BENCHMARKS];

#ifdef DEBUG
void
printAr (int64_t * a, int64_t N)
{
	int64_t i, j, n;

	n = N;
	j = 0;
	for (i = 0; i < 256; i++)
	{
		if (i % perCacheLine == 0)
		{
			printf ("#%" PRIu64 " a[%" PRIu64 "] =%" PRIu64 " \n", j, i, a[i]);
			j++;
		}
	}
	printf ("\n");
}
#endif
/*

void
printTimeAr ()
{
	int i, j;
	printf ("\n    ");
	for (i = 0; i < 4; i++)
		printf ("Start Stop  ");
	printf ("\n");
	for (i = 0; i < (id.maxThreads); i++)
	{
		printf ("t%d ", i);
		for (j = 0; j < (BENCHMARKS * 2); j++)
		{
			if (timeAr[i][j] > begin)
			{
				printf (" %5.2f", timeAr[i][j] - begin);
			}
			else
			{
				printf (" %5.2f", timeAr[i][j]);
			}
		}
		printf ("\n");
	}
	printf ("\n");
} */

void
zero_bandwidth (struct idThreadParams id)
{
	int i, array_size, num_array;
	array_size = maxMemory / sizeof (double);	/* in KB, start small */
	num_array = 0;
	while (array_size >= minMemory / sizeof (double))
	{
		for (cur_threads = 0; cur_threads < id.maxThreads; cur_threads++)
		{
			for (i = 0; i < BENCHMARKS; i++)
			{
				bandwidthAr[cur_threads][num_array][i] = 0;
			}
		}
		array_size = array_size * increaseArray;
		num_array++;
	}
}

void
print_bandwidth (char *str, struct idThreadParams id)
{
	FILE *fp;

	int i, array_size, num_array;
	array_size = maxMemory / sizeof (double);	/* in KB, start small */
	num_array = 0;
	fp = fopen (str, "w");
	fprintf
		(fp,
		 "#minMemory=%d maxMemory=%" PRIu64
		 " minThreads=%d maxThreads=%d writing to %s band=%d lat=%d\n",
		 minMemory, maxMemory, id.minThreads, id.maxThreads, str, band, lat);
	fprintf (fp,
				"#increaseArray=%f timestep=%f cacheSize=%" PRIu64
				" cacheLineSize=%d\n", increaseArray, timeStep, cacheSize,
				cacheLineSize);
	fprintf (fp, "#affinity=%d affinity_wide=%d\n", affinity, affinity_wide);
	fprintf (fp, "#numPages=%d\n", numPages);

	while (array_size >= minMemory / sizeof (double))
	{
		fprintf (fp, "ar= %8.2f ", array_size / 128.0);
		for (cur_threads = 0; cur_threads <= logint (id.maxThreads); cur_threads++)
		{
			for (i = 0; i < BENCHMARKS; i++)
			{
				fprintf (fp, "%7.2f ", bandwidthAr[cur_threads][num_array][i]);
			}
		}
		fprintf (fp, "\n");
		array_size = array_size * increaseArray;
		num_array++;
	}
	fclose (fp);
}

void
bandwidth_time (double *times, double *results, int64_t maxmem, long long scale,
					 int cur_threads)
{
	int i;
	double bandwidth;
	printf ("diff=%8.7f ", times[0]);
	for (i = 0; i < 2; i++)
	{
		bandwidth = ((maxmem / 1024.0) * cur_threads * scale) / times[i];
		bandwidth = bandwidth / 1024.0;	/* convert KB to MB. */
		if (i == 0)
			printf ("add = %6.2f MB/sec ", bandwidth);
		if (i == 1)
			printf ("triad = %6.2f MB/sec", bandwidth);
		results[i] = bandwidth;
	}
}


void
latency_time (double *times, double *results, int64_t maxmem, int scale,
				  int cur_threads)
{
	int64_t hops;
	double diff;
	double lat, avgLat;

	diff = times[0];
	hops = (maxmem / cacheLineSize) - 1;
	lat = 1.0e+9 * diff / (hops * cur_threads);
	lat = lat / scale;
	avgLat = 1.0e+9 * diff / hops / (int) scale;
	printf (" diff=%4.3f lat = %f avgLat = %f hops=%"PRIu64"", diff, lat,
			  avgLat, hops);
	results[0] = lat;
	results[1] = avgLat;
}

void *
stream_thread (void *arg)
{
	int i, j,pieces,offset;
	double *a, *b, *c;
	double *aa = NULL, *bb = NULL, *cc = NULL;
	int size;
	double scalar;
	struct idThreadParams *id = arg;
#ifdef VERBOSE
	printf ("id=%d maxThreads=%d\n",id->id, id->maxThreads);
#endif

#ifdef USEAFFINITY
	if (affinity)
		set_affinity (id);
#endif
	size = (maxmem / sizeof (double)) / 3;
	if (usenuma)
	{
#ifdef USENUMA
		if (!affinity) { // use numa affinity binding 
			pid_t pid=getpid();
			int aid=id->id;
			if (affinity_wide && spread>1) {
				aid=((aid%spread) * max_cpu/spread) + aid/spread;
			}
			struct bitmask *pBM=numa_bitmask_alloc(numa_num_configured_cpus());
			pBM=numa_bitmask_clearall(pBM);
			pBM=numa_bitmask_setbit(pBM, aid);
			numa_sched_setaffinity(pid,pBM);
		}
	//numa_run_on_node (id->id % CPUS);
#endif
		/* split the cache into thirds, and insure that each array maps
		   into it's 3rd.  Helps quite a bit on shanhai. */
#ifdef USENUMA
//		printf ("on cpu %d freeing %d\n",id->id);
		aa =
			(double *) numa_alloc_local (size * sizeof (double) + 2 * cacheSize +
			2 * cacheLineSize);
		bb =
			(double *) numa_alloc_local (size * sizeof (double) + 2 * cacheSize +
			2 * cacheLineSize);
		cc =
			(double *) numa_alloc_local (size * sizeof (double) + 2 * cacheSize +
			2 * cacheLineSize);
#endif
	}
	else
	{
		aa = (double *) malloc (size * sizeof (double) + 2 * cacheSize +
									 2 * cacheLineSize);
		bb = (double *) malloc (size * sizeof (double) + 2 * cacheSize +
									 2 * cacheLineSize);
		cc = (double *) malloc (size * sizeof (double) + 2 * cacheSize +
									 2 * cacheLineSize);
	}
	if ((aa == NULL) || (bb == NULL) || (cc == NULL))
	{
		printf ("allocation of array of %d doubles failes\n", size);
		printf ("aa=%p bb=%p cc=%p\n", (void *) aa, (void *) bb, (void *) cc);
		exit (-1);
	}
	/* align each pointer with their 1/3rd of the cache */

// need to change 12 to maxthreads * 3
// 12 is perfect for 4 threads.
	if (shared_cache)
	{
		/* devide shared cache into piece for each thread */
		pieces=id->maxThreads*3;
		offset=id->id*3;
	} else {
		/* assume each thread get's it's own cache */
		pieces=3;
		offset=0;
	}	
//	printf ("pieces = %d offset=%d cachesize=%d\n",pieces,offset,cacheSize);
	a = (double *) align_pointer ((int64_t *) aa, cacheSize, cacheLineSize, 
				pieces,offset+0);
	b = (double *) align_pointer ((int64_t *) bb, cacheSize, cacheLineSize, 
				pieces,offset+1);
	c = (double *) align_pointer ((int64_t *) cc, cacheSize, cacheLineSize, 
				pieces,offset+2);
/*
	a=(double *) aa;
	b=(double *) bb;
	c=(double *) cc;
*/

/*   printf ("a=%p acs=%p acls=%ld ps=%lL2=%" PRIu64 "\n",
           (void *) a, (long int) a % cacheSize, (lui) a % cacheLineSize,
           cacheSize);
   printf ("b=%p bcs=%p bcls=%ld L2=%" PRIu64 "\n",
           (void *) b, (long int) b % cacheSize, (lui) b % cacheLineSize,
           cacheSize);
   printf ("c=%p ccs=%p ccls=%ld L2=%" PRIu64 "\n",
           (void *) c, (long int) c % cacheSize, (lui) c % cacheLineSize,
           cacheSize); */

	if ((a == NULL) || (b == NULL) || (c == NULL))
	{
		printf ("allocation of array of %d doubles failed\n", size);
		printf ("a=%p b=%p c=%p\n", (void *) a, (void *) b, (void *) c);
		exit (-1);
	}

/*the below seems like a good idea, but fails in many environments
  ret=posix_memalign (&a,64,size * sizeof (double));
  ret=posix_memalign (&b,64,size * sizeof (double));
  ret=posix_memalign (&c,64,size * sizeof (double)); */
	for (i = 0; i < size; i++)
	{
		a[i] = 2.0;
		b[i] = 0.5;
		c[i] = 0.0;
	}
	scalar = 0.5 * a[1];
	sync_thread (id->id, label[0]);
	timeAr[id->id][0] = second ();
	for (j = 0; j < scale; j++)
	{
		switch (j % 2)
		{
		case 0:
			for (i = 0; i < size; i++)
			{
				c[i] = a[i] + b[i];
			}
			break;
		case 1:
			for (i = 0; i < size; i++)
			{
				b[i] = a[i] + c[i];
			}
		}
	}
	timeAr[id->id][1] = second ();
	for (i = 0; i < size; i++)
	{
		if (c[i] == 3.14159)
		{
			printf ("foo\n");
		}
		if (b[i] == 3.14159)
		{
			printf ("foo\n");
		}
	}
	sync_thread (id->id, label[1]);
	timeAr[id->id][2] = second ();
	for (j = 0; j < scale; j++)
	{
		switch (j % 2)
		{
		case 0:
			for (i = 0; i < size; i++)
			{
				a[i] = b[i] + scalar * c[i];
			}
			break;
		case 1:
			for (i = 0; i < size; i++)
			{
				a[i] = b[i] + scalar * c[i];
			}
		}
	}
	timeAr[id->id][3] = second ();
	for (i = 0; i < size; i++)
	{
		if (c[i] == 3.14159)
		{
			printf ("foo\n");
		}
		if (b[i] == 3.14159)
		{
			printf ("foo\n");
		}
	}
/*	printf ("diff=%f scale=%d size=%d\n", timeAr[id][3]-  timeAr[id][2] ,scale,size); */
	/* Do not allow free's to slow down other threads with work to do. */
	sync_thread (id->id, label[2]);
	if (usenuma)
	{
#ifdef USENMA
		printf ("freeing %d\n",size * sizeof (double) + 2 * cacheSize + 2 * cacheLineSize);
		numa_free (aa, size * sizeof (double) + 2 * cacheSize + 2 * cacheLineSize);
		numa_free (bb, size * sizeof (double) + 2 * cacheSize + 2 * cacheLineSize);
		numa_free (cc, size * sizeof (double) + 2 * cacheSize + 2 * cacheLineSize);
#endif
	}
	else
	{
		free (aa);
		free (bb);
		free (cc);
	}
	pthread_exit (NULL);
	return NULL;
}

void
help (char *argv[],struct idThreadParams id)
{
	printf ("Usage: %s <options>\n", argv[0]);
	printf ("  [-a ] use sched_setaffinity, default off\n");
	printf ("  [-A ] use sched_setaffinity striped across CPUs default off\n");
	printf
		("  [-c <set cache size in k bytes to align to>] default %" PRIu64
		 ", set to zero to disable\n", cacheSize / 1024);
	printf ("  [-f <filename to write data to>\n");
	printf ("  [-i <what percentage to shrink the array>] default %f\n",
			  increaseArray * 100.0);
	printf ("  [-t <maximum number of threads>] default %d\n", id.minThreads);
	printf ("  [-T <minimum number of threads>] default %d\n", id.maxThreads);
	printf ("  [-m <minimum array size in K>] default %d\n", minMemory / 1024);
	printf ("  [-M <maximum array size in M>] default %" PRIu64 "\n",
			  maxMemory / 1024);
	printf
		("  [-p <number of pages] restricts most reads to within <N> pages, 0 disables\n");
	printf ("  [-s <how many seconds per timestep>] default %f\n", timeStep);
	printf ("  [--shared align arrays to be friendly to a shared cache\n");
	printf ("  [-U turn on NUMA (if compiled in), default %d\n", usenuma);
	printf ("  [-u turn off NUMA (if compiled in), default %d\n", usenuma);
	printf ("  [-z <set cacheline size in bytes>] default %d\n",
			  cacheLineSize);
}

char *
fToStringDec (float x, char *result)
{
	int shift = 0;
	char *postfix = " kmgtp";
	while (x > 1000)
	{
		x = x / 1000;
		shift++;
	}
	if (x < 10)
		sprintf (result, "%4.3f%c", x, postfix[shift]);
	else if (x < 100)
		sprintf (result, "%4.2f%c", x, postfix[shift]);
	else
		sprintf (result, "%4.1f%c", x, postfix[shift]);

	return (result);
}

char *
fToStringBin (float x, char *result)
{
	int shift = 0;
	char *postfix = "KMGTP";
	while (x > 1000.0)
	{
		x = x / 1024;
		shift++;
	}
	if (x < 10)
		sprintf (result, "%4.3f%c", x, postfix[shift]);
	else if (x < 100)
		sprintf (result, "%4.2f%c", x, postfix[shift]);
	else
		sprintf (result, "%4.1f%c", x, postfix[shift]);

	return (result);
}


int
main (int argc, char *argv[])
{
	double diff;
	double max, min;
	int64_t i, j, array_size, num_array;
	int ret = 0;
	pthread_t reader[MAX_THREADS];
/* debugging */
	double difft[2];
	double results[2];
/*   printf ("argc=%d\n",argc); */
	char *logfile = NULL;
	int c = 0;
	char result1[7], result2[7];
	struct idThreadParams id;
	struct idThreadParams tid[MAX_THREADS];

    id.minThreads = 1;
	max_cpu=sysconf(_SC_NPROCESSORS_ONLN);
	pageSize=sysconf(_SC_PAGESIZE);
	cacheLineSize=sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
	cacheSize=sysconf(_SC_LEVEL3_CACHE_SIZE);
	id.maxThreads = max_cpu;
	perCacheLine = cacheLineSize / sizeof (int64_t);
	cacheLinesPerPage = pageSize / cacheLineSize;
	results[0] = 0;
	results[1] = 0;

	zero_bandwidth (id);
	static struct option long_options[] =
	{
		{"shared",no_argument,&shared_cache,1},
		{"sockets",required_argument,0,'b'},
		{ 0,0,0,0 }
	};

	while (1)
	{
   	int option_index = 0;
		c= getopt_long (argc, argv, "AalbPUuc:f:M:m:i:n:p:r:s:S:t:T:z:v?h",long_options,&option_index);
		if (c == -1)
        break;
		switch (c)
		{
		case 0:
     /* If this option set a flag, do nothing else now. */
          if (long_options[option_index].flag != 0)
            break;
          printf ("option %s", long_options[option_index].name);
          if (optarg)
            printf (" with arg %s", optarg);
          printf ("\n");
          break;
		case 'a':
			affinity = 1;
#ifndef USEAFFINITY
			printf
				("Sorry, not compiled with affinity support, use -DAFFINITY\n");
			exit (-1);
#endif
			break;
		case 'A':
			affinity = 1;
			affinity_wide = 1;
#ifndef USEAFFINITY
			printf
				("Sorry, not compiled with affinity support, use -DAFFINITY\n");
			exit (-1);
#endif
			break;
		case 'b':
			if (lat == 1)
			{
				printf ("sorry we can only do latency or bandwidth, not both\n");
				exit (-1);
			}
			else
				band = 1;
			break;
		case 'c':
			cacheSize = atoi (optarg) * 1024;
			break;
		case 'S':
			spread = atoi (optarg) ;
			break;
		case 'f':
			logfile = optarg;
			break;
		case 'l':
			if (band == 1)
			{
				printf ("sorry we can only do latency or bandwidth, not both\n");
				exit (-1);
			}
			else
				lat = 1;
			break;
		case 'm':
			minMemory = atoi (optarg) * 1024;
			break;
		case 'M':
			maxMemory = (int64_t) atoi (optarg) * 1024 * 1024;
			break;
		case 'p':
			numPages = atoi (optarg);
			break;
		case 'i':
			increaseArray = atof (optarg) / 100.0;
			break;
		case 's':
			timeStep = atof (optarg);
			break;
		case 't':
			id.minThreads = atoi (optarg);
			break;
		case 'T':
			id.maxThreads = atoi (optarg);
			if (id.maxThreads > MAX_THREADS)
			{
				printf
					("Maximum number of threads exceeds the array size of %d.\n",
					 MAX_THREADS);
				printf
					("Please use a number lower value or recompile with a larger MAX_THREADS\n");
				exit (-1);
			}
			break;

		case 'U':
			usenuma = 1;
			break;
		case 'u':
			usenuma = 0;
			break;
		case 'z':
			cacheLineSize = atoi (optarg);
			break;
		case 'h':
		case '?':
		default:
			help (argv,id);
			exit (0);

		}
	}
	if (logfile == NULL)
	{
		printf ("You must specify a log file with -f\n");
		exit (-1);
	}
	if ((band + lat) != 1)
	{
		printf ("you must pick exactly 1 of bandwdth and latency testing\n");
		exit (-1);
	}
	printf
		("minMemory=%d maxMemory=%" PRIu64
		 " minThreads=%d maxThreads=%d writing to %s band=%d lat=%d\n",
		 minMemory, maxMemory, id.minThreads, id.maxThreads, logfile, band, lat);
	printf ("increaseArray=%f timestep=%f cacheSize=%" PRIu64
			  " cacheLineSize=%d\n", increaseArray, timeStep, cacheSize,
			  cacheLineSize);
	printf ("affinity=%d affinity_wide=%d shared=%d\n", affinity, affinity_wide,shared_cache);
	printf ("usenuma=%d numPages=%d\n", usenuma, numPages);

	begin = second ();
	cur_threads = id.minThreads;
	while (cur_threads <= id.maxThreads)
	{
		printf ("*** threads=%d\n", cur_threads);
		array_size = maxMemory;	/* start large and shrink to keep malloc happy */
		/* Insure that every array is an even multiple of the cacheline size */
		diff = timeStep;
		scale = REPEAT;
		num_array = 0;

		while (array_size >= minMemory)
		{
			scale = scale * (timeStep / diff);
			if (scale<1) {
				scale=1;
			}
			for (i = 0; i < MAX_THREADS; i++)
			{
				for (j = 0; j < (BENCHMARKS * 2); j++)
				{
				}
			}
			for (i = 0; i < cur_threads; i++)
			{
				tid[i].id = i;
				tid[i].maxThreads= cur_threads;
				/* maxmem = bytes per thread to use */
				maxmem = array_size / cur_threads;
				if (band == 1)
					ret =
						pthread_create (&(reader[i]), NULL, stream_thread, &tid[i]);
				if (lat == 1)
					ret =
						pthread_create (&(reader[i]), NULL, latency_thread, &tid[i]);

				if (ret != 0)
				{
					printf ("ret=%d, pthread_create failed!!\n", ret);
					exit (-1);
				}
				else
				{
/*			printf ("thread %d created ret=%d\n",i,ret); */
				}

			}
			for (i = 0; i < cur_threads; i++)
			{
				ret = pthread_join (reader[i], NULL);
#ifdef DEBUG
				printf ("join ret val=%d i=%d\n", ret, i);
#endif
			}

			printf ("%d Thread(s) size=%sB repeat=%s ", cur_threads,
					  fToStringBin (array_size / 1024.0, result1),
					  fToStringDec ((float) scale, result2));
			for (i = 0; i < BENCHMARKS; i++)
			{
/*	printf ("max=%f min=%f\n",DBL_MAX,DBL_MIN); */
#ifndef PGCC_BROKEN
				min = DBL_MAX;
				max = DBL_MIN;
#else
				min = 1.7976931348623157e+208;
				max = 2.2250738585072014e-208;
#endif

				for (j = 0; j < cur_threads; j++)
				{
					if (timeAr[j][i * 2] < min)
					{
						min = timeAr[j][i * 2];
					}
					if (timeAr[j][i * 2 + 1] > max)
					{
						max = timeAr[j][i * 2 + 1];
					}
				}
				difft[i] = max - min;
			}
			if (difft[0] > 0 && difft[1]>0) {
			diff = difft[0];
			if (lat == 1)
				latency_time (difft, results, maxmem, scale, cur_threads);
			if (band == 1)
				bandwidth_time (difft, results, maxmem, scale, cur_threads);
			bandwidthAr[logint (cur_threads)][num_array][0] = results[0];
			bandwidthAr[logint (cur_threads)][num_array][1] = results[1];
/*	      printf ("cur=%d index=%d\n", cur_threads, log[cur_threads]); */
			printf ("\n");
			}
			array_size = array_size * increaseArray;
			num_array++;
		}
		cur_threads = cur_threads * 2;
	}
	print_bandwidth (logfile,id);
	return (0);
}
