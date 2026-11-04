/*
 * vln: symbolic links on the phone (iOS has no ln). Built next to vivad (tools/vivad/build.sh), pushed to
 * /private/var/root/viva-tests/vln, run through the USB link:
 *   tools/viva-link.py run /private/var/root/viva-tests/vln TARGET LINK   replace LINK by a symlink to TARGET
 *   tools/viva-link.py run /private/var/root/viva-tests/vln -r LINK       print where LINK points
 * e.g. the time zone: vln /var/db/timezone/zoneinfo/Europe/Rome /private/var/db/timezone/localtime
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	char buf[1024];
	if (argc == 3 && strcmp(argv[1], "-r") == 0) {
		ssize_t n = readlink(argv[2], buf, sizeof(buf) - 1);
		if (n < 0) {
			printf("vln: %s: %s\n", argv[2], strerror(errno));
			return 1;
		}
		buf[n] = 0;
		printf("%s -> %s\n", argv[2], buf);
		return 0;
	}
	if (argc != 3) {
		printf("usage: vln TARGET LINK | vln -r LINK\n");
		return 2;
	}
	struct stat st;
	if (lstat(argv[2], &st) == 0 && S_ISDIR(st.st_mode)) {
		printf("vln: %s is a directory, not replaced\n", argv[2]);
		return 1;
	}
	if (lstat(argv[2], &st) == 0 && unlink(argv[2]) != 0) {
		printf("vln: unlink %s: %s\n", argv[2], strerror(errno));
		return 1;
	}
	if (symlink(argv[1], argv[2]) != 0) {
		printf("vln: symlink %s -> %s: %s\n", argv[2], argv[1], strerror(errno));
		return 1;
	}
	printf("%s -> %s\n", argv[2], argv[1]);
	return 0;
}
