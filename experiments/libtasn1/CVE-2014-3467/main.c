#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <klee/klee.h>

#include <libtasn1.h>

void run(unsigned int buf_size) {
    int result;
    asn1_node definitions = NULL;
    asn1_node element = NULL;
    char error[ASN1_MAX_ERROR_DESCRIPTION_SIZE];
    unsigned char *buf = NULL;

    result = asn1_parser2tree("def.asn", &definitions, error);
    if (result != ASN1_SUCCESS) {
        printf("asn1_parser2tree: %s\n", error);
        return;
    }

    result = asn1_create_element(definitions, "Protocol.Message", &element);
    if (result != ASN1_SUCCESS) {
        printf("asn1_create_element: %s\n", error);
        return;
    }

    fprintf(stderr, "buffer size = %u\n", buf_size);
    buf = malloc(buf_size);
    klee_make_symbolic(buf, buf_size, "buf");

    result = asn1_der_decoding(&element, buf, buf_size, error);
    if (result != ASN1_SUCCESS) {
        return;
    }
}

int main(int argc, char *argv[], char *envp[]) {
    if (argc != 2) {
        return 1;
    }

    unsigned int k = strtoul(argv[1], NULL, 10);
    run(k);

    return 0;
}
