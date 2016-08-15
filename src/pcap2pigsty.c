/*
 *                                Copyright (C) 2016 by Rafael Santiago
 *
 * This is a free software. You can redistribute it and/or modify under
 * the terms of the GNU General Public License version 2.
 *
 */
#include "pcap2pigsty.h"
#include "pcap.h"
#include "pktslicer.h"
#include <stdio.h>
#include <string.h>

typedef int (*pcap_rec_dumper)(FILE *pigsty, const pcap_record_ctx *record);

static pcap_rec_dumper g_pcap_rec_dumper_lt[0xff][0xff] = { 0 };

static pcap_rec_dumper g_pcap_rec_dumper_ip4tlayer_lt[0xff] = { 0 };

//static pcap_rec_dumper g_pcap_rec_dumper_ip6tlayer_lt[0xffff] = { 0 };

static int save_pcap_rec_chunk(FILE *pigsty, const pcap_record_ctx *record);

static void init_pcap_rec_dumper_lookup_table();

static int ip4_dumper(FILE *pigsty, const pcap_record_ctx *record);

static int arp_dumper(FILE *pigsty, const pcap_record_ctx *record);

static int generic_dumper(FILE *pigsty, const pcap_record_ctx *record);

static int tcp_dumper(FILE *pigsty, const pcap_record_ctx *record);

static int udp_dumper(FILE *pigsty, const pcap_record_ctx *record);

static int generic_ip4tlayer_dumper(FILE *pigsty, const pcap_record_ctx *record);

static void dump_xdata(FILE *pigsty, const char *field, const unsigned char *buffer, size_t buffer_size);

static void dump_ddata(FILE *pigsty, const char *field, const unsigned char *buffer, size_t buffer_size);

static void dump_ip4addr(FILE *pigsty, const char *field, const unsigned char *buffer, size_t buffer_size);

static void dump_xstring(FILE *pigsty, const char *field, const unsigned char *buffer, size_t buffer_size);

static void init_pcap_rec_dumper_lookup_tables() {
    static int ltdone = 0;

    if (ltdone) {
        return;
    }

    memset(g_pcap_rec_dumper_lt, 0, sizeof(g_pcap_rec_dumper_lt));

    g_pcap_rec_dumper_lt[0x08][0x00] = ip4_dumper;
    g_pcap_rec_dumper_lt[0x08][0x06] = arp_dumper;
    //g_pcap_rec_dumper_lt[0x08][0xdd] = ip6_dumper;
    g_pcap_rec_dumper_lt[0xff][0xff] = generic_dumper;

    memset(g_pcap_rec_dumper_ip4tlayer_lt, 0, sizeof(g_pcap_rec_dumper_ip4tlayer_lt));

    g_pcap_rec_dumper_ip4tlayer_lt[0x06] = tcp_dumper;
    g_pcap_rec_dumper_ip4tlayer_lt[0x11] = udp_dumper;
    g_pcap_rec_dumper_ip4tlayer_lt[0xff] = generic_ip4tlayer_dumper;

    //memset(g_pcap_rec_dumper_ip6tlayer_lt, 0, sizeof(g_pcap_rec_dumper_ip6tlayer_lt));

    //g_pcap_rec_dumper_ip6tlayer_lt[0x06] = tcp_dumper;
    //g_pcap_rec_dumper_ip6tlayer_lt[0x11] = udp_dumper;

    ltdone = 1;
}

int pcap2pigsty(const char *pigsty_filepath, const char *pcap_filepath) {
    int exit_code = 1;
    pcap_file_ctx *pcap = NULL;
    pcap_record_ctx *rp = NULL;
    FILE *pigsty = NULL;

    if (pigsty_filepath == NULL) {
        goto ___pcap2pigsty_cleanup;
    }

    if (pcap_filepath == NULL) {
        goto ___pcap2pigsty_cleanup;
    }

    if ((pcap = ld_pcap_file(pcap_filepath)) == NULL) {
        goto ___pcap2pigsty_cleanup;
    }

    if ((pigsty = fopen(pcap_filepath, "a")) == NULL) {
        goto ___pcap2pigsty_cleanup;
    }

    init_pcap_rec_dumper_lookup_tables();

    exit_code = 0;

    for (rp = pcap->rec; rp != NULL && exit_code == 0; rp = rp->next) {
        exit_code = save_pcap_rec_chunk(pigsty, rp);
    }

___pcap2pigsty_cleanup:

    if (pcap != NULL) {
        close_pcap_file(pcap);
    }

    if (pigsty != NULL) {
        fclose(pigsty);
    }

    return exit_code;
}

static int save_pcap_rec_chunk(FILE *pigsty, const pcap_record_ctx *record) {
    pcap_rec_dumper pktdumper = NULL;
    unsigned short *ethtype = NULL;

    if (pigsty == NULL || record == NULL || record->data == NULL) {
        return 1;
    }

    ethtype = get_pkt_field("eth.type", record->data, record->hdr.incl_len, NULL);

    if (ethtype == NULL) {
        return 1;
    }

    pktdumper = g_pcap_rec_dumper_lt[(*ethtype) >> 4][(*ethtype) & 0xff];

    if (pktdumper == NULL) {
        pktdumper = g_pcap_rec_dumper_lt[0xff][0xff];

        if (pktdumper == NULL) {  // WARN(Santiago): It should never happen.
            return 1;
        }
    }

    free(ethtype);

    return pktdumper(pigsty, record);
}

static int ip4_dumper(FILE *pigsty, const pcap_record_ctx *record) {
    struct ip4_dump_mm {
        const char *field;
        void (*write)(FILE *pigsty, const char *field, const unsigned char *buffer, const size_t buffer_size);
    };
    struct ip4_dump_mm dumper[] = {
        { "ip.version",  dump_ddata   },
        { "ip.ihl",      dump_xdata   },
        { "ip.tos",      dump_xdata   },
        { "ip.tlen",     dump_ddata   },
        { "ip.id",       dump_xdata   },
        { "ip.offset",   dump_xdata   },
        { "ip.ttl",      dump_ddata   },
        { "ip.protocol", dump_ddata   },
        { "ip.checksum", dump_xdata   },
        { "ip.src",      dump_ip4addr },
        { "ip.dst",      dump_ip4addr }
    };
    size_t dumper_size = sizeof(dumper) / sizeof(dumper[0]);
    size_t d = 0;
    void *buffer = NULL;
    size_t buffer_size = 0;
    const char *field = NULL;
    unsigned char tlayer = 0xff;
    pcap_rec_dumper tlayerdumper = NULL;

    if (pigsty == NULL || record == NULL) {
        return 1;
    }

    for (d = 0; d < dumper_size; d++) {
        if (dumper[d].write != NULL) {
            field = dumper[d].field;
            buffer = get_pkt_field(field, record->data, record->hdr.incl_len, &buffer_size);
            dumper[d].write(pigsty, field, buffer, buffer_size);

            if ((d + 1) != dumper_size) {
                fprintf(pigsty, ",");
            }

            if (strcmp(field, "ip.protocol") == 0) {
                tlayer = *(unsigned char *)buffer;
            }

            free(buffer);
        }
    }

    tlayerdumper = g_pcap_rec_dumper_ip4tlayer_lt[tlayer];

    if (tlayerdumper == NULL) {

        tlayerdumper = g_pcap_rec_dumper_ip4tlayer_lt[0xff];

        if (tlayerdumper == NULL) {  // WARN(Santiago): It should never happen.
            return 1;
        }

    }

    return tlayerdumper(pigsty, record);
}

static int arp_dumper(FILE *pigsty, const pcap_record_ctx *record) {
    return 0;
}

static int generic_dumper(FILE *pigsty, const pcap_record_ctx *record) {
    dump_xstring(pigsty, "eth.payload", record->data, record->hdr.incl_len);
    return 0;
}

static int tcp_dumper(FILE *pigsty, const pcap_record_ctx *record) {
    return 0;
}

static int udp_dumper(FILE *pigsty, const pcap_record_ctx *record) {
    return 0;
}

static int generic_ip4tlayer_dumper(FILE *pigsty, const pcap_record_ctx *record) {
    dump_xstring(pigsty, "ip.payload", record->data, record->hdr.incl_len);
    return 0;
}

static void dump_xdata(FILE *pigsty, const char *field, const unsigned char *buffer, size_t buffer_size) {
    char temp[20] = "";
    const unsigned char *bp = NULL;
    const unsigned char *bp_end = NULL;

    if (pigsty == NULL || field == NULL || buffer == NULL || buffer_size == 0) {
        return;
    }

    fprintf(pigsty, "\n\t%s = 0x", field);

    bp = buffer;
    bp_end = bp + buffer_size;
    while (bp != bp_end) {
        sprintf(temp, "%.2X", *bp);
        fprintf(pigsty, "%s", temp);
        bp++;
    }
}

static void dump_ddata(FILE *pigsty, const char *field, const unsigned char *buffer, size_t buffer_size) {
    char temp[20] = "";

    if (pigsty == NULL || field == NULL || buffer == NULL || buffer_size == 0) {
        return;
    }

    switch (buffer_size) {
        case sizeof(unsigned char):
            sprintf(temp, "%d", (unsigned char *)buffer);
            break;

        case sizeof(unsigned short):
            sprintf(temp, "%d", (unsigned short *)buffer);
            break;

        case sizeof(unsigned int):
            sprintf(temp, "%d", (unsigned int *)buffer);
            break;

        default:
            return;
    }

    fprintf(pigsty, "\n\t%s = %s", field, temp);
}

static void dump_ip4addr(FILE *pigsty, const char *field, const unsigned char *buffer, size_t buffer_size) {

    if (pigsty == NULL || field == NULL || buffer == NULL || buffer_size != 4) {
        return;
    }

    fprintf(pigsty, "\n\t%s = %d.%d.%d.%d", field, *(buffer), *(buffer + 1), *(buffer + 2), *(buffer + 3));
}

static void dump_xstring(FILE *pigsty, const char *field, const unsigned char *buffer, size_t buffer_size) {
    const char *bp = NULL;
    const char *bp_end = NULL;
    char temp[20] = "";

    if (pigsty == NULL || field == NULL || buffer == NULL || buffer_size == 0) {
        return;
    }

    bp = buffer;
    bp_end = bp + buffer_size;

    fprintf(pigsty, "\n\t%s = \"", field);

    while (bp != bp_end) {
        sprintf(temp, "\\x%.2X", *bp);
        fprintf(pigsty, "%s", temp);
        bp++;
    }

    fprintf(pigsty, "\"");
}
