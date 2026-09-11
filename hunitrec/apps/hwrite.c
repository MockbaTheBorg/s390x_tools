/*
 * hwrite.c -- send a text file to a Hercules unit-record write device
 * (/dev/hprt* printer or /dev/hpch* punch, from the hunitrec driver) one
 * line per write(2) call, so each line lands as its own CCW/print-line or
 * card -- a plain `cat file > /dev/hprtN` doesn't guarantee that framing.
 *
 * Compiled twice (see Makefile) with -DMAXLEN=132 as "hprint" and
 * -DMAXLEN=80 as "hpunch"; MAXLEN matches the driver's own per-kind cap
 * (tools/hunitrec/hunitrec.c), lines longer than that are truncated
 * exactly like the driver would truncate them anyway.
 *
 * Silent by default -- pass -v/--verbose for the "N line(s) sent" summary.
 * Errors always print regardless of -v.
 *
 * Usage: hprint|hpunch [-v|--verbose] <device> [file]   (default: stdin)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#ifndef MAXLEN
#define MAXLEN 132
#endif

int main(int argc, char **argv)
{
	const char *prog = argv[0];
	int verbose = 0;
	static struct option longopts[] = {
		{"verbose", no_argument, NULL, 'v'},
		{0, 0, 0, 0},
	};
	int c;

	while ((c = getopt_long(argc, argv, "v", longopts, NULL)) != -1) {
		switch (c) {
		case 'v':
			verbose = 1;
			break;
		default:
			fprintf(stderr, "usage: %s [-v|--verbose] <device> [file]\n", prog);
			return 2;
		}
	}
	argc -= optind;
	argv += optind;

	if (argc < 1 || argc > 2) {
		fprintf(stderr, "usage: %s [-v|--verbose] <device> [file]\n", prog);
		return 2;
	}

	const char *device = argv[0];
	int devfd = open(device, O_WRONLY);
	if (devfd < 0) {
		fprintf(stderr, "open %s: %s\n", device, strerror(errno));
		return 1;
	}

	FILE *in = (argc == 2) ? fopen(argv[1], "r") : stdin;
	if (!in) {
		fprintf(stderr, "open %s: %s\n", argv[1], strerror(errno));
		return 1;
	}

	char line[4096];
	int lines = 0, rc = 0;
	while (fgets(line, sizeof(line), in)) {
		size_t len = strlen(line);
		if (len && line[len - 1] == '\n')
			line[--len] = '\0';
		if (len > MAXLEN)
			len = MAXLEN;

		ssize_t w = write(devfd, line, len);
		if (w < 0) {
			fprintf(stderr, "write line %d: %s\n", lines + 1, strerror(errno));
			rc = 1;
			break;
		}
		lines++;
	}

	if (in != stdin)
		fclose(in);
	close(devfd);

	if (verbose)
		fprintf(stderr, "%s: %d line(s) sent to %s\n", prog, lines, device);
	return rc;
}
