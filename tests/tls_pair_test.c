#include "tls_pair.h"

#include <stdio.h>
#include <string.h>

int
main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s CERT KEY ok|fail\n", argv[0]);
        return 2;
    }

    char reason[64] = {0};
    int rc = tls_pair_validate(argv[1], argv[2], reason, sizeof(reason));

    if (strcmp(argv[3], "ok") == 0) {
        if (rc != 0) {
            fprintf(stderr, "expected valid pair, got: %s\n", reason);
            return 1;
        }
        printf("tls_pair_test: valid pair OK\n");
        return 0;
    }

    if (strcmp(argv[3], "fail") == 0) {
        if (rc == 0) {
            fprintf(stderr, "expected invalid pair, validator accepted it\n");
            return 1;
        }
        printf("tls_pair_test: rejected invalid pair (%s)\n", reason);
        return 0;
    }

    fprintf(stderr, "unknown expectation: %s\n", argv[3]);
    return 2;
}
