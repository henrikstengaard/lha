/* ------------------------------------------------------------------------ */
/* LHa for UNIX                                                             */
/*              slice.c -- sliding dictionary with percolating update       */
/*                                                                          */
/*      Modified                Nobutaka Watazaki                           */
/*                                                                          */
/*  Ver. 1.14d  Exchanging a search algorithm  1997.01.11    T.Okamoto      */
/* ------------------------------------------------------------------------ */

#if 0
#define DEBUG 1
#endif

#include "lha.h"

#ifdef DEBUG
FILE *fout = NULL;
static int noslide = 1;
#endif

/* ------------------------------------------------------------------------ */

static unsigned long encoded_origsize;

/* ------------------------------------------------------------------------ */

static unsigned int *hash;
static unsigned int *prev;

/* static unsigned char  *text; */
unsigned char *too_flag;

/* hash function: it represents 3 letters from `pos' on `text' */
#define INIT_HASH(pos) \
        ((( (text[(pos)] << 5) \
           ^ text[(pos) + 1]  ) << 5) \
           ^ text[(pos) + 2]         ) & (unsigned)(HSHSIZ - 1);
#define NEXT_HASH(hash,pos) \
        (((hash) << 5) \
           ^ text[(pos) + 2]         ) & (unsigned)(HSHSIZ - 1);

static struct encode_option encode_define[2] = {
#if defined(__STDC__) || defined(AIX)
    /* lh1 */
    {(void (*) ()) output_dyn,
     (void (*) ()) encode_start_fix,
     (void (*) ()) encode_end_dyn},
    /* lh4, 5,6 */
    {(void (*) ()) output_st1,
     (void (*) ()) encode_start_st1,
     (void (*) ()) encode_end_st1}
#else
    /* lh1 */
    {(int (*) ()) output_dyn,
     (int (*) ()) encode_start_fix,
     (int (*) ()) encode_end_dyn},
    /* lh4, 5,6 */
    {(int (*) ()) output_st1,
     (int (*) ()) encode_start_st1,
     (int (*) ()) encode_end_st1}
#endif
};

static struct decode_option decode_define[] = {
    /* lh1 */
    {decode_c_dyn, decode_p_st0, decode_start_fix},
    /* lh2 */
    {decode_c_dyn, decode_p_dyn, decode_start_dyn},
    /* lh3 */
    {decode_c_st0, decode_p_st0, decode_start_st0},
    /* lh4 */
    {decode_c_st1, decode_p_st1, decode_start_st1},
    /* lh5 */
    {decode_c_st1, decode_p_st1, decode_start_st1},
    /* lh6 */
    {decode_c_st1, decode_p_st1, decode_start_st1},
    /* lh7 */
    {decode_c_st1, decode_p_st1, decode_start_st1},
    /* lzs */
    {decode_c_lzs, decode_p_lzs, decode_start_lzs},
    /* lz5 */
    {decode_c_lz5, decode_p_lz5, decode_start_lz5}
};

static struct encode_option encode_set;
static struct decode_option decode_set;

#define TXTSIZ (MAX_DICSIZ * 2L + MAXMATCH)
#define HSHSIZ (((unsigned long)1) <<15)
#define NIL 0
#define LIMIT 0x100 /* chain 長の limit */

static unsigned int txtsiz;

static unsigned long dicsiz;

static unsigned int hval;
static int matchlen;
static unsigned int matchpos;
static unsigned int pos;
static unsigned int remainder;


/* ------------------------------------------------------------------------ */
int
encode_alloc(method)
    int             method;
{
    switch (method) {
    case LZHUFF1_METHOD_NUM:
        encode_set = encode_define[0];
        maxmatch = 60;
        dicbit = LZHUFF1_DICBIT;    /* 12 bits  Changed N.Watazaki */
        break;
    case LZHUFF5_METHOD_NUM:
        encode_set = encode_define[1];
        maxmatch = MAXMATCH;
        dicbit = LZHUFF5_DICBIT;    /* 13 bits */
        break;
    case LZHUFF6_METHOD_NUM:
        encode_set = encode_define[1];
        maxmatch = MAXMATCH;
        dicbit = LZHUFF6_DICBIT;    /* 15 bits */
        break;
    case LZHUFF7_METHOD_NUM:
        encode_set = encode_define[1];
        maxmatch = MAXMATCH;
        dicbit = LZHUFF7_DICBIT;    /* 16 bits */
        break;
    default:
        error("unknown method %d", method);
        exit(1);
    }

    dicsiz = (((unsigned long)1) << dicbit);
    txtsiz = dicsiz*2+maxmatch;

    if (hash) return method;

    alloc_buf();

    hash = (unsigned int*)xmalloc(HSHSIZ * sizeof(unsigned int));
    prev = (unsigned int*)xmalloc(MAX_DICSIZ * sizeof(unsigned int));
    text = (unsigned char*)xmalloc(TXTSIZ);
    too_flag = (unsigned char*)xmalloc(HSHSIZ);

    return method;
}

/* ------------------------------------------------------------------------ */
/* ポインタの初期化 */

static void init_slide()
{
    unsigned int i;

    for (i = 0; i < HSHSIZ; i++) {
        hash[i] = NIL;
        too_flag[i] = 0;
    }
}

/* 辞書を DICSIZ 分 前にずらす */

static void
update(crc)
    unsigned int *crc;
{
    unsigned int i, j;
    long n;

    memmove(&text[0], &text[dicsiz], txtsiz - dicsiz);

    n = fread_crc(crc, &text[txtsiz - dicsiz], dicsiz, infile);

    remainder += n;
    encoded_origsize += n;      /* total size of read bytes */

    pos -= dicsiz;
    for (i = 0; i < HSHSIZ; i++) {
        j = hash[i];
        hash[i] = (j > dicsiz) ? j - dicsiz : NIL;
        too_flag[i] = 0;
    }
    for (i = 0; i < dicsiz; i++) {
        j = prev[i];
        prev[i] = (j > dicsiz) ? j - dicsiz : NIL;
    }
}


/* 現在の文字列をチェーンに追加する */

static void insert()
{
    prev[pos & (dicsiz - 1)] = hash[hval];
    hash[hval] = pos;
}


/* 現在の文字列と最長一致する文字列を検索し、チェーンに追加する */

static void match_insert()
{
    unsigned int scan_pos, scan_end, len;
    unsigned char *a, *b;
    unsigned int chain, off, h, max;

    max = maxmatch; /* MAXMATCH; */
    if (matchlen < THRESHOLD - 1) matchlen = THRESHOLD - 1;
    matchpos = pos;

    off = 0;
    for (h = hval; too_flag[h] && off < maxmatch - THRESHOLD; ) {
        ++off;
        h = NEXT_HASH(h, pos+off);
    }
    if (off == maxmatch - THRESHOLD) off = 0;
    for (;;) {
        chain = 0;
        scan_pos = hash[h];
        scan_end = (pos > dicsiz) ? pos + off - dicsiz : off;
        while (scan_pos > scan_end) {
            chain++;

            if (text[scan_pos + matchlen - off] == text[pos + matchlen]) {
                {
                    a = text + scan_pos - off;  b = text + pos;
                    for (len = 0; len < max && *a++ == *b++; len++);
                }

                if (len > matchlen) {
                    matchpos = scan_pos - off;
                    if ((matchlen = len) == max) {
                        break;
                    }
#ifdef DEBUG
                    if (noslide) {
                      if (matchpos < dicsiz) {
                        printf("matchpos=%u scan_pos=%u dicsiz=%u\n"
                               ,matchpos, scan_pos, dicsiz);
                      }
                    }
#endif
                }
            }
            scan_pos = prev[scan_pos & (dicsiz - 1)];
        }

        if (chain >= LIMIT)
            too_flag[h] = 1;

        if (matchlen > off + 2 || off == 0)
            break;
        max = off + 2;
        off = 0;
        h = hval;
    }
    prev[pos & (dicsiz - 1)] = hash[hval];
    hash[hval] = pos;
}


/* ポインタを進め、辞書を更新し、ハッシュ値を更新する */

static void
get_next(crc)
    unsigned int *crc;
{
    remainder--;
    if (++pos >= txtsiz - maxmatch) {
        update(crc);
#ifdef DEBUG
        noslide = 0;
#endif
    }
    hval = NEXT_HASH(hval, pos);
}

unsigned int
encode(interface)
    struct interfacing *interface;
{
    int lastmatchlen;
    unsigned int lastmatchoffset;
    unsigned int crc;

#ifdef DEBUG
    unsigned int addr;

    addr = 0;

    fout = fopen("en", "wt");
    if (fout == NULL) exit(1);
#endif
    infile = interface->infile;
    outfile = interface->outfile;
    origsize = interface->original;
    compsize = count = 0L;
    unpackable = 0;

    INITIALIZE_CRC(crc);

    /* encode_alloc(); */ /* allocate_memory(); */
    init_slide();  

    encode_set.encode_start();
    memset(&text[0], ' ', TXTSIZ);

    remainder = fread_crc(&crc, &text[dicsiz], txtsiz-dicsiz, infile);
    encoded_origsize = remainder;
    matchlen = THRESHOLD - 1;

    pos = dicsiz;

    if (matchlen > remainder) matchlen = remainder;
    hval = INIT_HASH(pos);

    insert();
    while (remainder > 0 && ! unpackable) {
        lastmatchlen = matchlen;  lastmatchoffset = pos - matchpos - 1;
        --matchlen;
        get_next(&crc);  match_insert();
        if (matchlen > remainder) matchlen = remainder;
        if (matchlen > lastmatchlen || lastmatchlen < THRESHOLD) {
            encode_set.output(text[pos - 1], 0);
#ifdef DEBUG
            fprintf(fout, "%u C %02X\n", addr, text[pos-1]);
            addr++;
#endif
            count++;
        } else {
            encode_set.output(lastmatchlen + (UCHAR_MAX + 1 - THRESHOLD),
               (lastmatchoffset) & (dicsiz-1) );
            --lastmatchlen;

#ifdef DEBUG
            fprintf(fout, "%u M %u %u ", addr,
                    lastmatchoffset & (dicsiz-1), lastmatchlen+1);
            addr += lastmatchlen +1 ;

            {
                int t,cc;
                for (t=0; t<lastmatchlen+1; t++) {
                    cc = text[pos - lastmatchoffset - 2 + t];
                    fprintf(fout, "%02X ", cc);
                }
                fprintf(fout, "\n");
            }
#endif
            while (--lastmatchlen > 0) {
                get_next(&crc);  insert();
                count++;
            }
            get_next(&crc);
            matchlen = THRESHOLD - 1;
            match_insert();
            if (matchlen > remainder) matchlen = remainder;
        }
    }
    encode_set.encode_end();

    interface->packed = compsize;
    interface->original = encoded_origsize;

    return crc;
}

/* ------------------------------------------------------------------------ */
unsigned int
decode(interface)
    struct interfacing *interface;
{
    unsigned int i, j, k, c;
    unsigned int dicsiz1, offset;
    unsigned char *dtext;
    unsigned int crc;

#ifdef DEBUG
    fout = fopen("de", "wt");
    if (fout == NULL) exit(1);
#endif

    infile = interface->infile;
    outfile = interface->outfile;
    dicbit = interface->dicbit;
    origsize = interface->original;
    compsize = interface->packed;
    decode_set = decode_define[interface->method - 1];

    INITIALIZE_CRC(crc);
    prev_char = -1;
    dicsiz = 1L << dicbit;
    dtext = (unsigned char *)xmalloc(dicsiz);
    for (i=0; i<dicsiz; i++) dtext[i] = 0x20;
    decode_set.decode_start();
    dicsiz1 = dicsiz - 1;
    offset = (interface->method == LARC_METHOD_NUM) ? 0x100 - 2 : 0x100 - 3;
    count = 0;
    loc = 0;
    while (count < origsize) {
        c = decode_set.decode_c();
        if (c <= UCHAR_MAX) {
#ifdef DEBUG
          fprintf(fout, "%u C %02X\n", count, c);
#endif
            dtext[loc++] = c;
            if (loc == dicsiz) {
                fwrite_crc(&crc, dtext, dicsiz, outfile);
                loc = 0;
            }
            count++;
        }
        else {
            j = c - offset;
            i = (loc - decode_set.decode_p() - 1) & dicsiz1;
#ifdef DEBUG
            fprintf(fout, "%u M %u %u ", count, (loc-1-i) & dicsiz1, j);
#endif
            count += j;
            for (k = 0; k < j; k++) {
                c = dtext[(i + k) & dicsiz1];

#ifdef DEBUG
                fprintf(fout, "%02X ", c & 0xff);
#endif
                dtext[loc++] = c;
                if (loc == dicsiz) {
                    fwrite_crc(&crc, dtext, dicsiz, outfile);
                    loc = 0;
                }
            }
#ifdef DEBUG
            fprintf(fout, "\n");
#endif
        }
    }
    if (loc != 0) {
        fwrite_crc(&crc, dtext, loc, outfile);
    }

    free(dtext);
    return crc;
}
