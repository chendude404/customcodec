#include "compression.h"
#include "glx_huff_tables.h"   /* generated: glx_huff_len_mu1[], glx_huff_len_mu0[] */

/*
Canonical Huffman for the delta alphabet. All tables (encode codes + decode
walk) are derived from code lengths, so encoder and decoder stay consistent
whether the lengths come from the designed static tables or from the length
block embedded in a .glx file. See compression.h for the overall flow.
*/

const uint8_t *glx_huff_lengths_for(int mulaw)
{
    return mulaw ? glx_huff_len_mu1 : glx_huff_len_mu0;
}

int glx_huff_build(GlxHuffTable *t, const uint8_t len[GLX_HUFF_NSYM])
{
    int maxlen = 0;
    for (int i = 0; i < GLX_HUFF_NSYM; i++) {
        if (len[i] > GLX_HUFF_MAXLEN) return -1;
        t->len[i] = len[i];
        if (len[i] > maxlen) maxlen = len[i];
    }
    if (maxlen < 1) return -1;
    t->maxlen = maxlen;

    for (int l = 0; l <= maxlen; l++) t->count[l] = 0;
    for (int i = 0; i < GLX_HUFF_NSYM; i++)
        if (len[i] > 0) t->count[len[i]]++;

    /* Kraft equality: a complete prefix code satisfies
     * sum_l count[l] * 2^(maxlen-l) == 2^maxlen. Reject anything else. */
    long total = 0;
    for (int l = 1; l <= maxlen; l++)
        total += (long)t->count[l] << (maxlen - l);
    if (total != (1L << maxlen)) return -1;

    /* smallest canonical code for each length, then assign in symbol order */
    uint16_t next_code[GLX_HUFF_MAXLEN + 1];
    uint16_t code = 0;
    for (int bits = 1; bits <= maxlen; bits++) {
        code = (uint16_t)((code + t->count[bits - 1]) << 1);
        next_code[bits] = code;
    }
    for (int sym = 0; sym < GLX_HUFF_NSYM; sym++)
        t->code[sym] = (len[sym] > 0) ? next_code[len[sym]]++ : 0;

    /* symbols ordered by (length, symbol) — the order the decoder walks */
    int idx = 0;
    for (int l = 1; l <= maxlen; l++)
        for (int sym = 0; sym < GLX_HUFF_NSYM; sym++)
            if (len[sym] == l) t->symbols[idx++] = (uint8_t)sym;

    return 0;
}

void glx_huff_pack_lengths(const uint8_t len[GLX_HUFF_NSYM],
                           uint8_t out[GLX_HUFF_TABLE_BYTES])
{
    for (int i = 0; i < GLX_HUFF_TABLE_BYTES; i++) out[i] = 0;
    for (int i = 0; i < GLX_HUFF_NSYM; i++) {
        uint8_t v = len[i] & 0xF;                       /* lengths <= 14 fit */
        if (i & 1) out[i >> 1] |= v;                    /* low nibble  */
        else       out[i >> 1] |= (uint8_t)(v << 4);    /* high nibble */
    }
}

void glx_huff_unpack_lengths(const uint8_t in[GLX_HUFF_TABLE_BYTES],
                             uint8_t len[GLX_HUFF_NSYM])
{
    for (int i = 0; i < GLX_HUFF_NSYM; i++) {
        uint8_t b = in[i >> 1];
        len[i] = (i & 1) ? (b & 0xF) : (uint8_t)(b >> 4);
    }
}

size_t glx_huff_encode(BitPacker *bp, const GlxHuffTable *t, uint32_t sym,
                       uint8_t *out, size_t cap)
{
    return bitpack_write(bp, t->code[sym], t->len[sym], out, cap);
}

bool glx_huff_decode(BitUnpacker *bu, const GlxHuffTable *t, uint32_t *sym_out)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= t->maxlen; len++) {
        uint32_t bit;
        if (!bitunpack_read(bu, 1, &bit)) return false;
        code |= (int)bit;
        int count = t->count[len];
        if (code - first < count) {
            *sym_out = t->symbols[index + (code - first)];
            return true;
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return false;   /* over-long code: table/stream corrupt */
}
