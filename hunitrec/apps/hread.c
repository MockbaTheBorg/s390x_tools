/*
 * hread.c -- read cards from a Hercules card-reader device (/dev/hrdr*,
 * from the hunitrec driver) until EOF (out of cards), one 80-column card
 * per read(2) call, trailing spaces stripped, one line per card on
 * stdout/outfile.
 *
 * A read() while no deck has been submitted yet (Hera's reader panel
 * "connects" only for the duration of one submit) looks identical to a
 * real end-of-deck EOF at the driver level -- both come back as 0 bytes.
 * -b/--blocking tells hread to treat an EOF seen *before* any card has
 * arrived as "not submitted yet" and poll/retry instead of stopping; once
 * the first card lands it reverts to normal end-on-EOF behavior, so it
 * naturally waits for, then drains, exactly one submission and exits.
 *
 * Silent by default -- pass -v/--verbose for the "waiting..."/"N card(s)
 * read" messages. Errors always print regardless of -v.
 *
 * Usage: hread [-b|--blocking] [-v|--verbose] <device> [outfile]   (default: stdout)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#define CARD_LEN 80
#define POLL_INTERVAL_US 300000	/* 300ms between retries while waiting */

int main(int argc, char **argv)
{
	const char *prog = argv[0];
	int blocking = 0, verbose = 0;
	static struct option longopts[] = {
		{"blocking", no_argument, NULL, 'b'},
		{"verbose", no_argument, NULL, 'v'},
		{0, 0, 0, 0},
	};
	int c;

	while ((c = getopt_long(argc, argv, "bv", longopts, NULL)) != -1) {
		switch (c) {
		case 'b':
			blocking = 1;
			break;
		case 'v':
			verbose = 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-b|--blocking] [-v|--verbose] <device> [outfile]\n", prog);
			return 2;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1 || argc > 2) {
		fprintf(stderr, "usage: %s [-b|--blocking] [-v|--verbose] <device> [outfile]\n", prog);
		return 2;
	}

	const char *device = argv[0];
	int devfd = open(device, O_RDONLY);
	if (devfd < 0) {
		fprintf(stderr, "open %s: %s\n", device, strerror(errno));
		return 1;
	}

	FILE *out = (argc == 2) ? fopen(argv[1], "w") : stdout;
	if (!out) {
		fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}

	if (blocking && verbose)
		fprintf(stderr, "%s: waiting for a deck to be submitted...\n", prog);

	char card[CARD_LEN];
	int cards = 0;
	for (;;) {
		ssize_t n = read(devfd, card, sizeof(card));
		if (n < 0) {
			fprintf(stderr, "read card %d: %s\n", cards + 1, strerror(errno));
			close(devfd);
			return 1;
		}
		if (n == 0) {
			if (blocking && cards == 0) {
				usleep(POLL_INTERVAL_US);
				continue;
			}
			break;	/* out of cards */
		}

		while (n > 0 && card[n - 1] == ' ')
			n--;
		fwrite(card, 1, n, out);
		fputc('\n', out);
		cards++;
	}

	close(devfd);
	if (out != stdout)
		fclose(out);

	if (verbose)
		fprintf(stderr, "%s: %d card(s) read from %s\n", prog, cards, device);
	return 0;
}
