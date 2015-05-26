/*
 * Copyright © 2015 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software and
 * its documentation for any purpose is hereby granted without fee, provided
 * that the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of the copyright holders not be used in
 * advertising or publicity pertaining to distribution of the software
 * without specific, written prior permission.  The copyright holders make
 * no representations about the suitability of this software for any
 * purpose.  It is provided "as is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <filter.h>
#include <getopt.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
print_ptraccel_deltas(struct motion_filter *filter, double step)
{
	struct normalized_coords motion;
	uint64_t time = 0;
	double i;

	printf("# gnuplot:\n");
	printf("# set xlabel dx unaccelerated\n");
	printf("# set ylabel dx accelerated\n");
	printf("# set style data lines\n");
	printf("# plot \"gnuplot.data\" using 1:2 title \"step %.2f\"\n", step);
	printf("#\n");

	/* Accel flattens out after 15 and becomes linear */
	for (i = 0.0; i < 15.0; i += step) {
		motion.x = i;
		motion.y = 0;
		time += 12; /* pretend 80Hz data */

		motion = filter_dispatch(filter, &motion, NULL, time);

		printf("%.2f	%.3f\n", i, motion.x);
	}
}

static void
print_ptraccel_movement(struct motion_filter *filter,
			int nevents,
			double max_dx,
			double step)
{
	struct normalized_coords motion;
	uint64_t time = 0;
	double dx;
	int i;

	printf("# gnuplot:\n");
	printf("# set xlabel \"event number\"\n");
	printf("# set ylabel \"delta motion\"\n");
	printf("# set style data lines\n");
	printf("# plot \"gnuplot.data\" using 1:2 title \"dx out\", \\\n");
	printf("#      \"gnuplot.data\" using 1:3 title \"dx in\"\n");
	printf("#\n");

	if (nevents == 0) {
		if (step > 1.0)
			nevents = max_dx;
		else
			nevents = 1.0 * max_dx/step + 0.5;

		/* Print more events than needed so we see the curve
		 * flattening out */
		nevents *= 1.5;
	}

	dx = 0;

	for (i = 0; i < nevents; i++) {
		motion.x = dx;
		motion.y = 0;
		time += 12; /* pretend 80Hz data */

		filter_dispatch(filter, &motion, NULL, time);

		printf("%d	%.3f	%.3f\n", i, motion.x, dx);

		if (dx < max_dx)
			dx += step;
	}
}

static void
print_ptraccel_sequence(struct motion_filter *filter,
			int nevents,
			double *deltas)
{
	struct normalized_coords motion;
	uint64_t time = 0;
	double *dx;
	int i;

	printf("# gnuplot:\n");
	printf("# set xlabel \"event number\"\n");
	printf("# set ylabel \"delta motion\"\n");
	printf("# set style data lines\n");
	printf("# plot \"gnuplot.data\" using 1:2 title \"dx out\", \\\n");
	printf("#      \"gnuplot.data\" using 1:3 title \"dx in\"\n");
	printf("#\n");

	dx = deltas;

	for (i = 0; i < nevents; i++, dx++) {
		motion.x = *dx;
		motion.y = 0;
		time += 12; /* pretend 80Hz data */

		filter_dispatch(filter, &motion, NULL, time);

		printf("%d	%.3f	%.3f\n", i, motion.x, *dx);
	}
}

static void
print_accel_func(struct motion_filter *filter)
{
	double vel;

	printf("# gnuplot:\n");
	printf("# set xlabel \"speed\"\n");
	printf("# set ylabel \"raw accel factor\"\n");
	printf("# set style data lines\n");
	printf("# plot \"gnuplot.data\" using 1:2\n");
	for (vel = 0.0; vel < 3.0; vel += .0001) {
		double result = pointer_accel_profile_linear(filter,
                                                             NULL,
                                                             vel,
                                                             0 /* time */);
		printf("%.4f\t%.4f\n", vel, result);
	}
}

static void
usage(const char *argv0)
{
	printf("Usage: %s [options] [dx1] [dx2] [...] > gnuplot.data\n", argv0);
	printf("\n"
	       "Options:\n"
	       "--mode=<motion|accel|delta|sequence> \n"
	       "	motion   ... print motion to accelerated motion (default)\n"
	       "	delta    ... print delta to accelerated delta\n"
	       "	accel    ... print accel factor\n"
	       "	sequence ... print motion for custom delta sequence\n"
	       "--maxdx=<double>\n  ... in motion mode only. Stop increasing dx at maxdx\n"
	       "--steps=<double>\n  ... in motion and delta modes only. Increase dx by step each round\n"
	       "--speed=<double>\n  ... accel speed [-1, 1], default 0\n"
	       "\n"
	       "If extra arguments are present and mode is not given, mode defaults to 'sequence'\n"
	       "and the arguments are interpreted as sequence of delta x coordinates\n"
	       "\n"
	       "If stdin is a pipe, mode defaults to 'sequence' and the pipe is read \n"
	       "for delta coordinates\n"
	       "\n"
	       "Output best viewed with gnuplot. See output for gnuplot commands\n");
}

int
main(int argc, char **argv)
{
	struct motion_filter *filter;
	double step = 0.1,
	       max_dx = 10;
	int nevents = 0;
	bool print_accel = false,
	     print_motion = true,
	     print_delta = false,
	     print_sequence = false;
	double custom_deltas[1024];
	double speed = 0.0;
	enum {
		OPT_MODE = 1,
		OPT_NEVENTS,
		OPT_MAXDX,
		OPT_STEP,
		OPT_SPEED,
	};

	filter = create_pointer_accelerator_filter(pointer_accel_profile_linear);
	assert(filter != NULL);

	while (1) {
		int c;
		int option_index = 0;
		static struct option long_options[] = {
			{"mode", 1, 0, OPT_MODE },
			{"nevents", 1, 0, OPT_NEVENTS },
			{"maxdx", 1, 0, OPT_MAXDX },
			{"step", 1, 0, OPT_STEP },
			{"speed", 1, 0, OPT_SPEED },
			{0, 0, 0, 0}
		};

		c = getopt_long(argc, argv, "",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case OPT_MODE:
			if (streq(optarg, "accel"))
				print_accel = true;
			else if (streq(optarg, "motion"))
				print_motion = true;
			else if (streq(optarg, "delta"))
				print_delta = true;
			else if (streq(optarg, "sequence"))
				print_sequence = true;
			else {
				usage(argv[0]);
				return 1;
			}
			break;
		case OPT_NEVENTS:
			nevents = atoi(optarg);
			if (nevents == 0) {
				usage(argv[0]);
				return 1;
			}
			break;
		case OPT_MAXDX:
			max_dx = strtod(optarg, NULL);
			if (max_dx == 0.0) {
				usage(argv[0]);
				return 1;
			}
			break;
		case OPT_STEP:
			step = strtod(optarg, NULL);
			if (step == 0.0) {
				usage(argv[0]);
				return 1;
			}
			break;
		case OPT_SPEED:
			speed = strtod(optarg, NULL);
			break;
		default:
			usage(argv[0]);
			exit(1);
			break;
		}
	}

	filter_set_speed(filter, speed);

	if (!isatty(STDIN_FILENO)) {
		char buf[12];
		print_sequence = true;
		print_motion = false;
		nevents = 0;
		memset(custom_deltas, 0, sizeof(custom_deltas));

		while(fgets(buf, sizeof(buf), stdin) && nevents < 1024) {
			custom_deltas[nevents++] = strtod(buf, NULL);
		}
	} else if (optind < argc) {
		print_sequence = true;
		print_motion = false;
		nevents = 0;
		memset(custom_deltas, 0, sizeof(custom_deltas));
		while (optind < argc)
			custom_deltas[nevents++] = strtod(argv[optind++], NULL);
	}

	if (print_accel)
		print_accel_func(filter);
	else if (print_delta)
		print_ptraccel_deltas(filter, step);
	else if (print_motion)
		print_ptraccel_movement(filter, nevents, max_dx, step);
	else if (print_sequence)
		print_ptraccel_sequence(filter, nevents, custom_deltas);

	filter_destroy(filter);

	return 0;
}
