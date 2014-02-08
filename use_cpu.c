/*
 *  use_cpu -  A program for using specific amounts of CPU.
 *  Copyright (C) 2012  MontaVista Software, LLC <source@mvista.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <pthread.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/time.h>

char *help_opt = "\
  use_cpu [opts]\n\
Use cpu time on the system\n\
Options are:\n\
  --percent - Specify the default percentage to use on each CPU.  If this is\n\
      not specified the default is 50%.\n\
  --prio - Specify the priority to run the threads at.  This is a real-time\n\
      priority, or 0 for normal priority.  Default it 0.\n\
  --cpus    - Specify the CPUs to run on, and optionally the percent per CPU.\n\
      The format is <cpu#>[:<percent>][,<cpu#>[:<percent>]]...\n\
      If this is not specified then the default is all CPUs at the default\n\
      percentage.\n\
  --duration - The amount of time in seconds to run the test.  0 means\n\
      forever.  Default is 0.\n\
  --nogettodspin - Normally the program spins in each thread calling\n\
      gettimeofday.  If using LTT, this can generate a lot of noise.\n\
      With this, it uses a volatile to control spinning and that gets set\n\
      from the main thread.  This is not quite as reliable, and it may\n\
      have issues when getting close to 100%.\n\
";

struct option opts[] =
{
	{ "percent",	1, NULL, 'p' },
	{ "prio",	1, NULL, 'r' },
	{ "cpus",	1, NULL, 'c' },
	{ "duration",   1, NULL, 'd' },
	{ "nogettodspin", 0, NULL, 's' },
	{ NULL }
};

void
exit_err(char *format, ...)
{
	va_list ap;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	putc('\n', stdout);
	va_end(ap);
	exit(1);
}

struct proc {
	unsigned int num;
	int          valid;
	int          percent_given;
	int          use;
	struct procinfo *info;
	unsigned int prio;
	double       percent;
	pthread_t    thread;
	volatile unsigned int missed_sched;
	unsigned int last_missed_sched;
	volatile int waiting;
};

struct procinfo {
	unsigned int maxproc;
	int use_end_time;
	struct timeval end_time;
	volatile int end_now;
	struct proc *procs;
};

struct procinfo *g_pinfo;

void end_op_sig(int sig)
{
	g_pinfo->end_now = 1;
}

#define CPUINFO "/proc/cpuinfo"

struct procinfo *read_processors(void)
{
	FILE *f = fopen(CPUINFO, "r");
	char line[81];
	char *endptr;
	char *rv;
	unsigned int lineno = 0;
	unsigned int i;
	struct procinfo *pinfo;

	pinfo = malloc(sizeof(*pinfo));
	if (!pinfo)
		exit_err("Out of memory");
	pinfo->maxproc = 0;

	if (!f)
		exit_err("Unable to open %s", CPUINFO);

	/* Find the maximum processor id on this system. */
	rv = fgets(line, sizeof(line), f);
	while (rv) {
		lineno++;
		if (strncmp(line, "processor", 9) == 0) {
			char *p = strchr(line, ':');
			int unsigned v;
			if (!p)
				exit_err("%s:%u - Invalid processor line"
					 " format", CPUINFO, lineno);
			p++;
			while (isspace(*p))
				p++;
			if (strlen(p) == 0)
				exit_err("%s:%u - Invalid processor line"
					 " format", CPUINFO, lineno);
			v = strtoul(p, &endptr, 10);
			if (*endptr != '\n')
				exit_err("%s:%u - Invalid processor line"
					 " format", CPUINFO, lineno);
			if (v > pinfo->maxproc)
				pinfo->maxproc = v;
		}
		rv = fgets(line, sizeof(line), f);
	}

	/* Now allocate memory and fill in the valid processors. */
	pinfo->procs = malloc(sizeof(*pinfo->procs) * (pinfo->maxproc + 1));
	if (!pinfo)
		exit_err("Out of memory");
	for (i = 0; i <= pinfo->maxproc; i++) {
		pinfo->procs[i].num = i;
		pinfo->procs[i].valid = 0;
		pinfo->procs[i].percent_given = 0;
		pinfo->procs[i].use = 0;
		pinfo->procs[i].info = pinfo;
		pinfo->procs[i].prio = 0;
		pinfo->procs[i].missed_sched = 0;
		pinfo->procs[i].last_missed_sched = 0;
		pinfo->procs[i].waiting = 0;
	}
	rewind(f);
	lineno = 0;
	rv = fgets(line, sizeof(line), f);
	while (rv > 0) {
		lineno++;
		if (strncmp(line, "processor", 9) == 0) {
			char *p = strchr(line, ':');
			int unsigned v;
			if (!p)
				exit_err("%s:%u - Invalid processor line"
					 " format", CPUINFO, lineno);
			p++;
			while (isspace(*p))
				p++;
			if (strlen(p) == 0)
				exit_err("%s:%u - Invalid processor line"
					 " format", CPUINFO, lineno);
			v = strtoul(p, &endptr, 10);
			if (*endptr != '\n')
				exit_err("%s:%u - Invalid processor line"
					 " format", CPUINFO, lineno);
			if (v > pinfo->maxproc)
				exit_err("%s:%u - Processor number after max",
					CPUINFO, lineno);
			pinfo->procs[v].valid = 1;
		}
		rv = fgets(line, sizeof(line), f);
	}

	return pinfo;
}

void handle_cpus(struct procinfo *pinfo, char *cpuspec)
{
	char *ncpu;
	unsigned int count = 0;

	while (*cpuspec != '\0') {
		unsigned int n;
		char *endptr;

		count++;
		ncpu = strchr(cpuspec, ',');
		if (!ncpu)
			ncpu = cpuspec + strlen(cpuspec);
		n = strtoul(cpuspec, &endptr, 10);
		if (endptr == cpuspec)
			exit_err("cpu specifier %u is invalid", count);
		if (n > pinfo->maxproc)
			exit_err("cpu specifier %u is larger than the number"
				" of processors", count);
		if (!pinfo->procs[n].valid)
			exit_err("cpu specifier %u is not a valid processor",
				count);
		pinfo->procs[n].use = 1;
		if (*endptr == ':') {
			cpuspec = endptr + 1;
			pinfo->procs[n].percent = (strtod(cpuspec, &endptr)
						   / 100.0);
			if (endptr == cpuspec)
				exit_err("cpu specifier %u is invalid", count);
			if (pinfo->procs[n].percent >= 1.0)
				exit_err("cpu percent must be < 100");
			pinfo->procs[n].percent_given = 1;
		}
		cpuspec = endptr;
		if (*cpuspec == ',')
			cpuspec++;
		else if (*endptr != '\0')
			exit_err("cpu specifier %u is invalid", count);
	}
}

long subtime(struct timeval *l, struct timeval *r)
{
	return (((l->tv_sec - r->tv_sec) * 1000000) +
		(l->tv_usec - r->tv_usec));
}

int use_spin;
volatile int spin_now;

void *use_cpu(void *arg)
{
	struct proc *p = arg;
	struct procinfo *pinfo = p->info;
	struct timeval now, end;
	long time_per_sec;
	cpu_set_t set;
	int rv;

	CPU_ZERO(&set);
	CPU_SET(p->num, &set);
	rv = pthread_setaffinity_np(p->thread, sizeof(set), &set);
	if (rv)
		exit_err("pthread_setaffinity_np: %s", strerror(rv));
	time_per_sec = 1000000 * p->percent;
	gettimeofday(&now, NULL);
	p->waiting = 1;
	while (!pinfo->end_now && (!pinfo->use_end_time
				   || subtime(&now, &pinfo->end_time) < 0)) {
		/* Wait til the top of the second. */
		usleep(1000000 - now.tv_usec);
		p->waiting = 0;

		end.tv_sec = now.tv_sec + 1;
		end.tv_usec = time_per_sec;
		if (use_spin) {
			while (spin_now)
				;
			gettimeofday(&now, NULL);
		} else {
			gettimeofday(&now, NULL);
			while (subtime(&now, &end) < 0)
				gettimeofday(&now, NULL);
		}
		if (now.tv_sec > end.tv_sec)
			p->missed_sched++;
		p->waiting = 1;
	}
	return NULL;
}

void start_proc(struct proc *p)
{
	int rv;
	pthread_attr_t attr;
	struct sched_param param;
	int policy;

	rv = pthread_attr_init(&attr);
	if (rv)
		exit_err("pthread_attr_init: %s", strerror(rv));
	rv = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	if (rv)
		exit_err("pthread_attr_setinheritsched: %s",
			 strerror(rv));
	param.sched_priority = p->prio;
	if (p->prio > 0)
		policy = SCHED_FIFO;
	else
		policy = SCHED_OTHER;
	rv = pthread_setschedparam(pthread_self(), policy, &param);
	if (rv)
		exit_err("pthread_setschedparam: %s", strerror(rv));
	rv = pthread_attr_setschedpolicy(&attr, policy);
	if (rv)
		exit_err("pthread_attr_setschedpolicy: %s",
			 strerror(rv));
	rv = pthread_attr_setschedparam(&attr, &param);
	if (rv)
		exit_err("pthread_attr_setschedparam: %s",
			 strerror(rv));
	rv = pthread_create(&p->thread, &attr, use_cpu, p);
	if (rv)
		exit_err("pthread_create: %s", strerror(rv));
	rv = pthread_attr_destroy(&attr);
	if (rv)
		exit_err("pthread_attr_destroy: %s", strerror(rv));
}

int main(int argc, char *argv[])
{
	double default_percent = 0.5;
	char *endptr;
	struct procinfo *pinfo = read_processors();
	unsigned int i;
	int opt;
	int got_cpuspec = 0;
	unsigned int default_priority = 0;
	unsigned int duration = 0;
	struct timeval now;
	long time_per_sec;

	while ((opt = getopt_long(argc, argv, "p:c:r:d:s", opts, NULL)) != -1) {
		switch (opt) {
		case 'p':
			default_percent = strtod(optarg, &endptr) / 100.0;
			if (*endptr != '\0')
				exit_err("Invalid percent: %s", optarg);
			if (default_percent >= 1.0)
				exit_err("Percent must be < 100");
			break;

		case 'r':
			default_priority = strtoul(optarg, &endptr, 10);
			if (*endptr != '\0')
				exit_err("Invalid priority: %s", optarg);
			break;

		case 'd':
			duration = strtoul(optarg, &endptr, 10);
			if (*endptr != '\0')
				exit_err("Invalid duration: %s", optarg);
			break;

		case 'c':
			handle_cpus(pinfo, optarg);
			got_cpuspec = 1;
			break;

		case 's':
			use_spin = 1;
			break;

		case '?':
			puts(help_opt);
			exit(1);
		}
	}

	pinfo->end_now = 0;
	if (duration == 0) {
		pinfo->use_end_time = 0;
		memset(&pinfo->end_time, 0, sizeof(pinfo->end_time));
	} else {
		pinfo->use_end_time = 1;
		gettimeofday(&pinfo->end_time, NULL);
		pinfo->end_time.tv_sec += duration;
	}

	for (i = 0; i <= pinfo->maxproc; i++) {
		if (pinfo->procs[i].valid) {
			if (!pinfo->procs[i].percent_given)
				pinfo->procs[i].percent = default_percent;
			if (!got_cpuspec)
				pinfo->procs[i].use = 1;
			pinfo->procs[i].prio = default_priority;
		}
	}

	g_pinfo = pinfo;
	signal(SIGINT, end_op_sig);

	for (i = 0; i <= pinfo->maxproc; i++) {
		if (pinfo->procs[i].use)
			start_proc(&pinfo->procs[i]);
	}

	time_per_sec = 1000000 * default_percent;
	gettimeofday(&now, NULL);
	while (!pinfo->end_now && (!pinfo->use_end_time
				   || subtime(&now, &pinfo->end_time) < 0)) {
		/* Wait til the top of the second. */
		usleep(1000000 - now.tv_usec);

		if (use_spin) {
			struct timeval end;
			int not_waiting = 1;
			
			end.tv_sec = now.tv_sec + 1;
			end.tv_usec = time_per_sec;
			gettimeofday(&now, NULL);
			usleep(subtime(&end, &now));
			/* Tell everyone to stop. */
			spin_now = 0;
			/* Wait for them to stop and go into waiting mode. */
			while (not_waiting > 0) {
				not_waiting = 0;
				for (i = 0; i <= pinfo->maxproc; i++)
					not_waiting += !pinfo->procs[i].waiting;
			}
			/* Now let everyone go when they wake up next */
			spin_now = 1;
		}
		
		for (i = 0; i <= pinfo->maxproc; i++) {
			struct proc *p = &pinfo->procs[i];
			unsigned int missed = p->missed_sched;
			if (missed > p->last_missed_sched) {
				printf("Missed scheduling %d times on CPU %d\n",
				       missed - p->last_missed_sched,
				       i);
				p->last_missed_sched = p->missed_sched;
			}
		}

		gettimeofday(&now, NULL);
	}

	for (i = 0; i <= pinfo->maxproc; i++)
		pthread_join(pinfo->procs[i].thread, NULL);
	
	for (i = 0; i <= pinfo->maxproc; i++) {
		if (pinfo->procs[i].missed_sched)
			printf(" CPU %d: Missed scheduling %d times.\n",
			       i, pinfo->procs[i].missed_sched);
	}
	printf("Finished using CPU\n");
	return 0;
}
