/*
Copyright 2011 Google Inc. All Rights Reserved.
Copyright 2015 Frédéric Kayser. All Rights Reserved.
Copyright 2024 Mr_KrzYch00. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Author: lode.vandevenne@gmail.com (Lode Vandevenne)
Author: jyrki.alakuijala@gmail.com (Jyrki Alakuijala)
*/

#include "defines.h"
#include "deflate.h"

#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#if SC_PLATFORM == SC_PLATFORM_LINUX
#include <sched.h>
#include <sys/stat.h>
#include <errno.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
#ifdef __MACH__
#include "affinity.h"
#endif

#include "inthandler.h"
#include "blocksplitter.h"
#include "squeeze.h"
#include "symbols.h"
#include "tree.h"
#include "crc32.h"

/*
bp = bitpointer, always in range [0, 7].
The outsize is number of necessary bytes to encode the bits.
Given the value of bp and the amount of bytes, the amount of bits represented
is not simply bytesize * 8 + bp because even representing one bit requires a
whole byte. It is: (bp == 0) ? (bytesize * 8) : ((bytesize - 1) * 8 + bp)
*/
static void AddBit(int bit,
                   unsigned char* bp, unsigned char** out, size_t* outsize) {
  if (*bp == 0) ZOPFLI_APPEND_DATA(0, out, outsize);
  (*out)[*outsize - 1] |= bit << *bp;
  *bp = (*bp + 1) & 7;
}

static void AddBits(unsigned symbol, unsigned length,
                    unsigned char* bp, unsigned char** out, size_t* outsize) {
  /* TODO(lode): make more efficient (add more bits at once). */
  unsigned i;
  for (i = 0; i < length; i++) {
    unsigned bit = (symbol >> i) & 1;
    if (*bp == 0) ZOPFLI_APPEND_DATA(0, out, outsize);
    (*out)[*outsize - 1] |= bit << *bp;
    *bp = (*bp + 1) & 7;
  }
}

/*
Adds bits, like AddBits, but the order is inverted. The deflate specification
uses both orders in one standard.
*/
static void AddHuffmanBits(unsigned symbol, unsigned length,
                           unsigned char* bp, unsigned char** out,
                           size_t* outsize) {
  /* TODO(lode): make more efficient (add more bits at once). */
  unsigned i;
  for (i = 0; i < length; i++) {
    unsigned bit = (symbol >> (length - i - 1)) & 1;
    if (*bp == 0) ZOPFLI_APPEND_DATA(0, out, outsize);
    (*out)[*outsize - 1] |= bit << *bp;
    *bp = (*bp + 1) & 7;
  }
}

/*
Ensures there are at least 2 distance codes to support buggy decoders.
Zlib 1.2.1 and below have a bug where it fails if there isn't at least 1
distance code (with length > 0), even though it's valid according to the
deflate spec to have 0 distance codes. On top of that, some mobile phones
require at least two distance codes. To support these decoders too (but
potentially at the cost of a few bytes), add dummy code lengths of 1.
References to this bug can be found in the changelog of
Zlib 1.2.2 and here: http://www.jonof.id.au/forum/index.php?topic=515.0.

d_lengths: the 32 lengths of the distance codes.
*/
static void PatchDistanceCodesForBuggyDecoders(unsigned* d_lengths) {
  int num_dist_codes = 0; /* Amount of non-zero distance codes */
  size_t i;
  for (i = 0; i < 30 /* Ignore the two unused codes from the spec */; i++) {
    if (d_lengths[i]) num_dist_codes++;
    if (num_dist_codes >= 2) return; /* Two or more codes is fine. */
  }

  if (num_dist_codes == 0) {
    d_lengths[0] = d_lengths[1] = 1;
  } else if (num_dist_codes == 1) {
    d_lengths[d_lengths[0] ? 1 : 0] = 1;
  }
}

/*
Encodes the Huffman tree and returns how many bits its encoding takes. If out
is a null pointer, only returns the size and runs faster.
Here we also support --ohh switch to Optimize Huffman Headers, code by
Frédéric Kayser.
*/
static size_t EncodeTree(const unsigned* ll_lengths,
                         const unsigned* d_lengths,
                         int use_16, int use_17, int use_18, int fuse_8, int fuse_7,
                         /* TODO replace those by single int */
                         unsigned char* bp,
                         unsigned char** out, size_t* outsize, int ohh, int revcounts) {
  unsigned lld_total;  /* Total amount of literal, length, distance codes. */
  /* Runlength encoded version of lengths of litlen and dist trees. */
  unsigned* rle = 0;
  unsigned* rle_bits = 0;  /* Extra bits for rle values 16, 17 and 18. */
  size_t rle_size = 0;  /* Size of rle array. */
  size_t rle_bits_size = 0;  /* Should have same value as rle_size. */
  unsigned hlit = 29;  /* 286 - 257 */
  unsigned hdist = 29;  /* 32 - 1, but gzip does not like hdist > 29.*/
  unsigned hclen;
  unsigned hlit2;
  size_t i, j;
  size_t clcounts[19];
  unsigned clcl[19];  /* Code length code lengths. */
  unsigned clsymbols[19];
  /* The order in which code length code lengths are encoded as per deflate. */
  static const unsigned order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
  };
  int size_only = !out;
  size_t result_size = 0;

  memset(clcounts, 0, 19 * sizeof(clcounts[0]));

  /* Trim zeros. */
  while (hlit > 0 && ll_lengths[257 + hlit - 1] == 0) hlit--;
  while (hdist > 0 && d_lengths[1 + hdist - 1] == 0) hdist--;
  hlit2 = hlit + 257;

  lld_total = hlit2 + hdist + 1;

  for (i = 0; i < lld_total; i++) {
    /* This is an encoding of a huffman tree, so now the length is a symbol */
    unsigned char symbol = i < hlit2 ? ll_lengths[i] : d_lengths[i - hlit2];
    unsigned count = 1;
    if(use_16 || (symbol == 0 && (use_17 || use_18))) {
      for (j = i + 1; j < lld_total && symbol ==
          (j < hlit2 ? ll_lengths[j] : d_lengths[j - hlit2]); j++) {
        count++;
      }
    }
    i += count - 1;

    /* Repetitions of zeroes */
    if (symbol == 0 && count >= 3) {
      if (use_18) {
        while (count >= 11) {
          unsigned count2 = count > 138 ? 138 : count;
          if (!size_only) {
            ZOPFLI_APPEND_DATA(18, &rle, &rle_size);
            ZOPFLI_APPEND_DATA(count2 - 11, &rle_bits, &rle_bits_size);
          }
          clcounts[18]++;
          count -= count2;
        }
      }
      if (use_17) {
        while (count >= 3) {
          unsigned count2 = count > 10 ? 10 : count;
          if (!size_only) {
            ZOPFLI_APPEND_DATA(17, &rle, &rle_size);
            ZOPFLI_APPEND_DATA(count2 - 3, &rle_bits, &rle_bits_size);
          }
          clcounts[17]++;
          count -= count2;
        }
      }
    }

    /* Repetitions of any symbol */
    if (use_16 && count >= 4) {
      count--;  /* Since the first one is hardcoded. */
      clcounts[symbol]++;
      if (!size_only) {
        ZOPFLI_APPEND_DATA(symbol, &rle, &rle_size);
        ZOPFLI_APPEND_DATA(0, &rle_bits, &rle_bits_size);
      }
      while (count >= 3) {
        if(ohh==0) {
          unsigned count2 = count > 6 ? 6 : count;
          if (!size_only) {
            ZOPFLI_APPEND_DATA(16, &rle, &rle_size);
            ZOPFLI_APPEND_DATA(count2 - 3, &rle_bits, &rle_bits_size);
          }
          clcounts[16]++;
          count -= count2;
        } else {
          if (fuse_8 && count == 8) { /* record 8 as 4+4 not as 6+single+single */
            if (!size_only) {
              ZOPFLI_APPEND_DATA(16, &rle, &rle_size);
              ZOPFLI_APPEND_DATA(1, &rle_bits, &rle_bits_size);
              ZOPFLI_APPEND_DATA(16, &rle, &rle_size);
              ZOPFLI_APPEND_DATA(1, &rle_bits, &rle_bits_size);
            }
            clcounts[16] += 2;
            count = 0;
          } else if (fuse_7 && count == 7) { /* record 7 as 4+3 not as 6+single */
            if (!size_only) {
              ZOPFLI_APPEND_DATA(16, &rle, &rle_size);
              ZOPFLI_APPEND_DATA(1, &rle_bits, &rle_bits_size);
              ZOPFLI_APPEND_DATA(16, &rle, &rle_size);
              ZOPFLI_APPEND_DATA(0, &rle_bits, &rle_bits_size);
            }
            clcounts[16] += 2;
            count = 0;
          } else {
            unsigned count2 = count > 6 ? 6 : count;
            if (!size_only) {
              ZOPFLI_APPEND_DATA(16, &rle, &rle_size);
              ZOPFLI_APPEND_DATA(count2 - 3, &rle_bits, &rle_bits_size);
            }
            clcounts[16]++;
            count -= count2;
          }
        }
      }
    }

    /* No or insufficient repetition */
    clcounts[symbol] += count;
    while (count > 0) {
      if (!size_only) {
        ZOPFLI_APPEND_DATA(symbol, &rle, &rle_size);
        ZOPFLI_APPEND_DATA(0, &rle_bits, &rle_bits_size);
      }
      count--;
    }
  }

  ZopfliCalculateBitLengths(clcounts, 19, 7, clcl, revcounts);
  if (!size_only) ZopfliLengthsToSymbols(clcl, 19, 7, clsymbols);

  hclen = 15;
  /* Trim zeros. */
  while (hclen > 0 && clcounts[order[hclen + 4 - 1]] == 0) hclen--;

  if (!size_only) {
    AddBits(hlit, 5, bp, out, outsize);
    AddBits(hdist, 5, bp, out, outsize);
    AddBits(hclen, 4, bp, out, outsize);

    for (i = 0; i < hclen + 4; i++) {
      AddBits(clcl[order[i]], 3, bp, out, outsize);
    }

    for (i = 0; i < rle_size; i++) {
      unsigned symbol = clsymbols[rle[i]];
      AddHuffmanBits(symbol, clcl[rle[i]], bp, out, outsize);
      /* Extra bits. */
      if (rle[i] == 16) AddBits(rle_bits[i], 2, bp, out, outsize);
      else if (rle[i] == 17) AddBits(rle_bits[i], 3, bp, out, outsize);
      else if (rle[i] == 18) AddBits(rle_bits[i], 7, bp, out, outsize);
    }
  }

  result_size += 14;  /* hlit, hdist, hclen bits */
  result_size += (hclen + 4) * 3;  /* clcl bits */
  for(i = 0; i < 19; i++) {
    result_size += clcl[i] * clcounts[i];
  }
  /* Extra bits. */
  result_size += clcounts[16] * 2;
  result_size += clcounts[17] * 3;
  result_size += clcounts[18] * 7;

  /* Note: in case of "size_only" these are null pointers so no effect. */
  free(rle_bits);
  free(rle);

  return result_size;
}

/*
Here we also support --ohh switch to Optimize Huffman Headers, code by
Frédéric Kayser.
*/
static void AddDynamicTree(const unsigned* ll_lengths,
                           const unsigned* d_lengths,
                           unsigned char* bp,
                           unsigned char** out, size_t* outsize, int ohh, int revcounts) {
  int i;
  int j = 1;
  int k = 4;
  int l = 0;
  int m = 0;
  int best = 0;
  size_t bestsize = 0;

  if(ohh) {
   j=4;
   k=1;
  }

  for(i = 0; i < 8; i++) {
    size_t size = EncodeTree(ll_lengths, d_lengths,
                             i & j, i & 2, i & k, 0, 0,
                             0, 0, 0, ohh, revcounts);
    if (bestsize == 0 || size < bestsize) {
      bestsize = size;
      best = i;
    }
  }

  if(ohh) {
    for(i = 4; i < 8; i++) {
      size_t size = EncodeTree(ll_lengths, d_lengths,
                               i & 4, i & 2, i & 1, 1, 0,
                               0, 0, 0, ohh, revcounts);
      if (size < bestsize) {
        bestsize = size;
        best = 8+i;
      }
    }
    for(i = 4; i < 8; i++) {
      size_t size = EncodeTree(ll_lengths, d_lengths,
                               i & 4, i & 2, i & 1, 0, 1,
                               0, 0, 0, ohh, revcounts);
      if (size < bestsize) {
        bestsize = size;
        best = 16+i;
      }
    }

    for(i = 4; i < 8; i++) {
      size_t size = EncodeTree(ll_lengths, d_lengths,
                               i & 4, i & 2, i & 1, 1, 1,
                               0, 0, 0, ohh, revcounts);
      if (size < bestsize) {
        bestsize = size;
        best = 24+i;
      }
    }
   l=best & 8;
   m=best & 16;
  }

  EncodeTree(ll_lengths, d_lengths,
             best & j, best & 2, best & k, l, m,
             bp, out, outsize, ohh, revcounts);

}

/*
Gives the exact size of the tree, in bits, as it will be encoded in DEFLATE.
*/
static size_t CalculateTreeSize(const unsigned* ll_lengths,
                                const unsigned* d_lengths, int ohh, int revcounts) {
  size_t result = 0;
  int i;
  int j = 1;
  int k = 4;

  if(ohh) {
   j=4;
   k=1;
  }

  for(i = 0; i < 8; i++) {
    size_t size = EncodeTree(ll_lengths, d_lengths,
                             i & j, i & 2, i & k, 0, 0,
                             0, 0, 0, ohh, revcounts);
    if (result == 0 || size < result) result = size;
  }

  if(ohh) {
    for(i = 4; i < 8; i++) {
      size_t size = EncodeTree(ll_lengths, d_lengths,
                               i & 4, i & 2, i & 1, 1, 0,
                               0, 0, 0, ohh, revcounts);
      if (size < result) result = size;
    }
    for(i = 4; i < 8; i++) {
      size_t size = EncodeTree(ll_lengths, d_lengths,
                               i & 4, i & 2, i & 1, 0, 1,
                               0, 0, 0, ohh, revcounts);
      if (size < result) result = size;
    }
    for(i = 4; i < 8; i++) {
      size_t size = EncodeTree(ll_lengths, d_lengths,
                               i & 4, i & 2, i & 1, 1, 1,
                               0, 0, 0, ohh, revcounts);
      if (size < result) result = size;
    }
  }

  return result;
}

/*
Adds all lit/len and dist codes from the lists as huffman symbols. Does not add
end code 256. expected_data_size is the uncompressed block size, used for
assert, but you can set it to 0 to not do the assertion.
*/
static void AddLZ77Data(const ZopfliLZ77Store* lz77,
                        size_t lstart, size_t lend,
                        size_t expected_data_size,
                        const unsigned* ll_symbols, const unsigned* ll_lengths,
                        const unsigned* d_symbols, const unsigned* d_lengths,
                        unsigned char* bp,
                        unsigned char** out, size_t* outsize) {
  size_t testlength = 0;
  size_t i;
#ifdef NDEBUG
  (void)expected_data_size;
#endif

  for (i = lstart; i < lend; i++) {
    unsigned dist = lz77->dists[i];
    unsigned litlen = lz77->litlens[i];
    if (dist == 0) {
      assert(litlen < 256);
      assert(ll_lengths[litlen] > 0);
      AddHuffmanBits(ll_symbols[litlen], ll_lengths[litlen], bp, out, outsize);
      testlength++;
    } else {
      unsigned lls = ZopfliGetLengthSymbol(litlen);
      unsigned ds = ZopfliGetDistSymbol(dist);
      assert(litlen >= 3 && litlen <= 288);
      assert(ll_lengths[lls] > 0);
      assert(d_lengths[ds] > 0);
      AddHuffmanBits(ll_symbols[lls], ll_lengths[lls], bp, out, outsize);
      AddBits(ZopfliGetLengthExtraBitsValue(litlen),
              ZopfliGetLengthExtraBits(litlen),
              bp, out, outsize);
      AddHuffmanBits(d_symbols[ds], d_lengths[ds], bp, out, outsize);
      AddBits(ZopfliGetDistExtraBitsValue(dist),
              ZopfliGetDistExtraBits(dist),
              bp, out, outsize);
      testlength += litlen;
    }
  }
  assert(expected_data_size == 0 || testlength == expected_data_size);
}

static void GetFixedTree(unsigned* ll_lengths, unsigned* d_lengths) {
  unsigned short i;
  for (i = 0; i < 144; i++) ll_lengths[i] = 8;
  for (i = 144; i < 256; i++) ll_lengths[i] = 9;
  for (i = 256; i < 280; i++) ll_lengths[i] = 7;
  for (i = 280; i < 288; i++) ll_lengths[i] = 8;
  for (i = 0; i < 32; i++) d_lengths[i] = 5;
}

/*
Same as CalculateBlockSymbolSize, but for block size smaller than histogram
size.
*/
static size_t CalculateBlockSymbolSizeSmall(const unsigned* ll_lengths,
                                            const unsigned* d_lengths,
                                            const ZopfliLZ77Store* lz77,
                                            size_t lstart, size_t lend) {
  size_t result = 0;
  size_t i;
  for (i = lstart; i < lend; i++) {
    assert(i < lz77->size);
    assert(lz77->litlens[i] < 259);
    if (lz77->dists[i] == 0) {
      result += ll_lengths[lz77->litlens[i]];
    } else {
      int ll_symbol = ZopfliGetLengthSymbol(lz77->litlens[i]);
      int d_symbol = ZopfliGetDistSymbol(lz77->dists[i]);
      result += ll_lengths[ll_symbol];
      result += d_lengths[d_symbol];
      result += ZopfliGetLengthSymbolExtraBits(ll_symbol);
      result += ZopfliGetDistSymbolExtraBits(d_symbol);
    }
  }
  result += ll_lengths[256]; /*end symbol*/
  return result;
}

/*
Same as CalculateBlockSymbolSize, but with the histogram provided by the caller.
*/
static size_t CalculateBlockSymbolSizeGivenCounts(const size_t* ll_counts,
                                                  const size_t* d_counts,
                                                  const unsigned* ll_lengths,
                                                  const unsigned* d_lengths,
                                                  const ZopfliLZ77Store* lz77,
                                                  size_t lstart, size_t lend) {
  size_t result = 0;
  size_t i;
  if (lstart + ZOPFLI_NUM_LL * 3 > lend) {
    return CalculateBlockSymbolSizeSmall(
        ll_lengths, d_lengths, lz77, lstart, lend);
  } else {
    for (i = 0; i < 256; i++) {
      result += ll_lengths[i] * ll_counts[i];
    }
    for (i = 257; i < 286; i++) {
      result += ll_lengths[i] * ll_counts[i];
      result += ZopfliGetLengthSymbolExtraBits(i) * ll_counts[i];
    }
    for (i = 0; i < 30; i++) {
      result += d_lengths[i] * d_counts[i];
      result += ZopfliGetDistSymbolExtraBits(i) * d_counts[i];
    }
    result += ll_lengths[256]; /*end symbol*/
    return result;
  }
}

/*
Calculates size of the part after the header and tree of an LZ77 block, in bits.
*/
static size_t CalculateBlockSymbolSize(const unsigned* ll_lengths,
                                       const unsigned* d_lengths,
                                       const ZopfliLZ77Store* lz77,
                                       size_t lstart, size_t lend) {
  if (lstart + ZOPFLI_NUM_LL * 3 > lend) {
    return CalculateBlockSymbolSizeSmall(
        ll_lengths, d_lengths, lz77, lstart, lend);
  } else {
    size_t ll_counts[ZOPFLI_NUM_LL];
    size_t d_counts[ZOPFLI_NUM_D];
    ZopfliLZ77GetHistogram(lz77, lstart, lend, ll_counts, d_counts);
    return CalculateBlockSymbolSizeGivenCounts(
        ll_counts, d_counts, ll_lengths, d_lengths, lz77, lstart, lend);
  }
}

static size_t AbsDiff(size_t x, size_t y) {
  if (x > y)
    return x - y;
  else
    return y - x;
}

/*
Changes the population counts in a way that the consequent Huffman tree
compression, especially its rle-part, will be more likely to compress this data
more efficiently. length contains the size of the histogram.
*/
static void OptimizeHuffmanForRle(unsigned int length, size_t* counts) {
  unsigned int i, k, stride;
  size_t symbol, sum, limit;
  unsigned char* good_for_rle;

  /* 1) We don't want to touch the trailing zeros. We may break the
  rules of the format by adding more data in the distance codes. */
  for (;; --length) {
    if (length == 0) {
      return;
    }
    if (counts[length - 1] != 0) {
      /* Now counts[0..length - 1] does not have trailing zeros. */
      break;
    }
  }
  /* 2) Let's mark all population counts that already can be encoded
  with an rle code.*/
  good_for_rle = Zcalloc(length, 1);

  /* Let's not spoil any of the existing good rle codes.
  Mark any seq of 0's that is longer than 5 as a good_for_rle.
  Mark any seq of non-0's that is longer than 7 as a good_for_rle.*/
  symbol = counts[0];
  stride = 0;
  for (i = 0; i < length + 1; ++i) {
    if (i == length || counts[i] != symbol) {
      if ((symbol == 0 && stride >= 5) || (symbol != 0 && stride >= 7)) {
        memset(good_for_rle + (i - stride), 1, stride);
      }
      stride = 1;
      if (i != length) {
        symbol = counts[i];
      }
    } else {
      ++stride;
    }
  }

  /* 3) Let's replace those population counts that lead to more rle codes. */
  stride = 0;
  limit = counts[0];
  sum = 0;
  for (i = 0; i < length + 1; ++i) {
    if (i == length || good_for_rle[i]
        /* Heuristic for selecting the stride ranges to collapse. */
        || AbsDiff(counts[i], limit) >= 4) {
      if (stride >= 4 || (stride >= 3 && sum == 0)) {
        /* The stride must end, collapse what we have, if we have enough (4). */
        int count = (sum + stride / 2) / stride;
        if (count < 1) count = 1;
        if (sum == 0) {
          /* Don't make an all zeros stride to be upgraded to ones. */
          count = 0;
        }
        for (k = 0; k < stride; ++k) {
          /* We don't want to change value at counts[i],
          that is already belonging to the next stride. Thus - 1. */
          counts[i - k - 1] = count;
        }
      }
      stride = 0;
      sum = 0;
      if (i < length - 3) {
        /* All interesting strides have a count of at least 4,
        at least when non-zeros. */
        limit = (counts[i] + counts[i + 1] +
                 counts[i + 2] + counts[i + 3] + 2) / 4;
      } else if (i < length) {
        limit = counts[i];
      } else {
        limit = 0;
      }
    }
    ++stride;
    if (i != length) {
      sum += counts[i];
    }
  }

  free(good_for_rle);
}

/*
Similar to above but implemented from Brotli. Exposed as --brotli switch
for more try&errors to get smallest possible results.
*/
static unsigned OptimizeHuffmanForRleBrotli(size_t length, size_t* counts) {
  size_t nonzero_count = 0;
  size_t stride;
  size_t limit;
  size_t sum;
  const size_t streak_limit = 1240;
  unsigned char* good_for_rle;
  /* 1) Let's make the Huffman code more compatible with rle encoding. */
  size_t i;
  for (i = 0; i < length; i++) {
    if (counts[i]) {
      ++nonzero_count;
    }
  }
  if (nonzero_count < 16) {
    return 1;
  }
  while (length != 0 && counts[length - 1] == 0) {
    --length;
  }
  if (length == 0) {
    return 1;  /* All zeros. */
  }
  /* Now counts[0..length - 1] does not have trailing zeros. */
  {
    size_t nonzeros = 0;
    size_t smallest_nonzero = 1 << 30;
    for (i = 0; i < length; ++i) {
      if (counts[i] != 0) {
        ++nonzeros;
        if (smallest_nonzero > counts[i]) {
          smallest_nonzero = counts[i];
        }
      }
    }
    if (nonzeros < 5) {
      /* Small histogram will model it well. */
      return 1;
    }
    {
      size_t zeros = length - nonzeros;
      if (smallest_nonzero < 4) {
        if (zeros < 6) {
          for (i = 1; i < length - 1; ++i) {
            if (counts[i - 1] != 0 && counts[i] == 0 && counts[i + 1] != 0) {
              counts[i] = 1;
            }
          }
        }
      }
    }
    if (nonzeros < 28) {
      return 1;
    }
  }
    /* 2) Let's mark all population counts that already can be encoded
  with an rle code. */
  good_for_rle = Zcalloc(length, 1);
  {
    /* Let's not spoil any of the existing good rle codes.
    Mark any seq of 0's that is longer as 5 as a good_for_rle.
    Mark any seq of non-0's that is longer as 7 as a good_for_rle. */
    size_t symbol = counts[0];
    size_t step = 0;
    for (i = 0; i <= length; ++i) {
      if (i == length || counts[i] != symbol) {
        if ((symbol == 0 && step >= 5) ||
            (symbol != 0 && step >= 7)) {
          memset(good_for_rle + (i - step), 1, step);
        }
        step = 1;
        if (i != length) {
          symbol = counts[i];
        }
      } else {
        ++step;
      }
    }
  }
  /* 3) Let's replace those population counts that lead to more rle codes.
  Math here is in 24.8 fixed point representation. */
  stride = 0;
  limit = 256 * (counts[0] + counts[1] + counts[2]) / 3 + 420;
  sum = 0;
  for (i = 0; i <= length; ++i) {
    if (i == length || good_for_rle[i] ||
        (i != 0 && good_for_rle[i - 1]) ||
        (256 * counts[i] - limit + streak_limit) >= 2 * streak_limit) {
      if (stride >= 4 || (stride >= 3 && sum == 0)) {
        size_t k;
        /* The stride must end, collapse what we have, if we have enough (4). */
        size_t count = (sum + stride / 2) / stride;
        if (count == 0) {
          count = 1;
        }
        if (sum == 0) {
          /* Don't make an all zeros stride to be upgraded to ones. */
          count = 0;
        }
        for (k = 0; k < stride; ++k) {
          /* We don't want to change value at counts[i],
          that is already belonging to the next stride. Thus - 1. */
          counts[i - k - 1] = count;
        }
      }
      stride = 0;
      sum = 0;
      if (i < length - 2) {
        /* All interesting strides have a count of at least 4,
        at least when non-zeros. */
        limit = 256 * (counts[i] + counts[i + 1] + counts[i + 2]) / 3 + 420;
      } else if (i < length) {
        limit = 256 * counts[i];
      } else {
        limit = 0;
      }
    }
    ++stride;
    if (i != length) {
      sum += counts[i];
      if (stride >= 4) {
        limit = (256 * sum + stride / 2) / stride;
      }
      if (stride == 4) {
        limit += 120;
      }
    }
  }
  free(good_for_rle);
  return 1;
}

/*
Tries out OptimizeHuffmanForRle for this block, if the result is smaller,
uses it, otherwise keeps the original. Returns size of encoded tree and data in
bits, not including the 3-bit block header.
*/
static zfloat TryOptimizeHuffmanForRle(
    const ZopfliLZ77Store* lz77, size_t lstart, size_t lend,
    const size_t* ll_counts, const size_t* d_counts,
    unsigned* ll_lengths, unsigned* d_lengths, int usebrotli, int ohh,
    int revcounts) {
  size_t ll_counts2[ZOPFLI_NUM_LL];
  size_t d_counts2[ZOPFLI_NUM_D];
  unsigned ll_lengths2[ZOPFLI_NUM_LL];
  unsigned d_lengths2[ZOPFLI_NUM_D];
  zfloat treesize;
  zfloat datasize;
  zfloat treesize2;
  zfloat datasize2;

  treesize = CalculateTreeSize(ll_lengths, d_lengths, ohh, revcounts);
  datasize = CalculateBlockSymbolSizeGivenCounts(ll_counts, d_counts,
      ll_lengths, d_lengths, lz77, lstart, lend);

  memcpy(ll_counts2, ll_counts, sizeof(ll_counts2));
  memcpy(d_counts2, d_counts, sizeof(d_counts2));
  if(usebrotli==1) {
    OptimizeHuffmanForRleBrotli(ZOPFLI_NUM_LL, ll_counts2);
    OptimizeHuffmanForRleBrotli(ZOPFLI_NUM_D, d_counts2);
  } else {
    OptimizeHuffmanForRle(ZOPFLI_NUM_LL, ll_counts2);
    OptimizeHuffmanForRle(ZOPFLI_NUM_D, d_counts2);
  }
  ZopfliCalculateBitLengths(ll_counts2, ZOPFLI_NUM_LL, 15, ll_lengths2, revcounts);
  ZopfliCalculateBitLengths(d_counts2, ZOPFLI_NUM_D, 15, d_lengths2, revcounts);
  PatchDistanceCodesForBuggyDecoders(d_lengths2);

  treesize2 = CalculateTreeSize(ll_lengths2, d_lengths2, ohh, revcounts);
  datasize2 = CalculateBlockSymbolSizeGivenCounts(ll_counts, d_counts,
      ll_lengths2, d_lengths2, lz77, lstart, lend);

  if (treesize2 + datasize2 < treesize + datasize) {
    memcpy(ll_lengths, ll_lengths2, sizeof(ll_lengths2));
    memcpy(d_lengths, d_lengths2, sizeof(d_lengths2));
    return treesize2 + datasize2;
  }
  return treesize + datasize;
}

/*
Calculates the bit lengths for the symbols for dynamic blocks. Chooses bit
lengths that give the smallest size of tree encoding + encoding of all the
symbols to have smallest output size. This are not necessarily the ideal Huffman
bit lengths. Returns size of encoded tree and data in bits, not including the
3-bit block header.
*/
static zfloat GetDynamicLengths(const ZopfliLZ77Store* lz77,
                                size_t lstart, size_t lend,
                                unsigned* ll_lengths, unsigned* d_lengths,
                                int usebrotli, int revcounts, int ohh) {
  size_t ll_counts[ZOPFLI_NUM_LL];
  size_t d_counts[ZOPFLI_NUM_D];

  ZopfliLZ77GetHistogram(lz77, lstart, lend, ll_counts, d_counts);
  ll_counts[256] = 1;  /* End symbol. */
  ZopfliCalculateBitLengths(ll_counts, ZOPFLI_NUM_LL, 15, ll_lengths, revcounts);
  ZopfliCalculateBitLengths(d_counts, ZOPFLI_NUM_D, 15, d_lengths, revcounts);
  PatchDistanceCodesForBuggyDecoders(d_lengths);
  return TryOptimizeHuffmanForRle(
      lz77, lstart, lend, ll_counts, d_counts, ll_lengths, d_lengths, usebrotli,
      ohh, revcounts);
}

static void PrintBlockSummary(unsigned long insize, unsigned long outsize,
                              unsigned long treesize) {

  fprintf(stderr, "Compressed block size: %lu (%luk) ",outsize, outsize / 1024);
  if(treesize>0) fprintf(stderr, "(tree: %lu) ",treesize);
  fprintf(stderr, "(unc: %lu)\n",insize);

}

void PrintSummary(unsigned long insize, unsigned long outsize, unsigned long deflsize) {

  if(insize>0) {
    unsigned long ratio_comp = 0;
    fprintf(stderr, "Input size: %lu (%luK)                         \n", insize, insize / 1024);
    if(outsize>0) {
      ratio_comp=outsize;
      fprintf(stderr, "Output size: %lu (%luK)\n", outsize, outsize / 1024);
    }
    if(deflsize>0) {
      if(ratio_comp==0) ratio_comp=deflsize;
      fprintf(stderr, "Deflate size: %lu (%luK)\n", deflsize, deflsize / 1024);
    }
    fprintf(stderr, "Ratio: %.3f%%\n\n", 100.0 * (1 - (zpfloat)ratio_comp / (zpfloat)insize));
  }

}

zfloat ZopfliCalculateBlockSize(const ZopfliOptions* options,
                                const ZopfliLZ77Store* lz77,
                                size_t lstart, size_t lend, int btype,
                                int expensivedyn) {
  unsigned ll_lengths[ZOPFLI_NUM_LL];
  unsigned d_lengths[ZOPFLI_NUM_D];

  zfloat result = 3; /* bfinal and btype bits */

  if (btype == 0) {
    size_t length = ZopfliLZ77GetByteRange(lz77, lstart, lend);
    size_t rem = length % 65535;
    size_t blocks = length / 65535 + (rem ? 1 : 0);
    /* An uncompressed block must actually be split into multiple blocks if it's
       larger than 65535 bytes long. Eeach block header is 5 bytes: 3 bits,
       padding, LEN and NLEN (potential less padding for first one ignored). */
    return blocks * 5 * 8 + length * 8;
  } else if(btype == 1) {
    GetFixedTree(ll_lengths, d_lengths);
    result += CalculateBlockSymbolSize(
        ll_lengths, d_lengths, lz77, lstart, lend);
  } else {
    if(expensivedyn == 0) {
      int usebrotli = (options->mode & 0x0008) == 0x0008;
      int revcounts = (options->mode & 0x0004) == 0x0004;
      int ohh = (options->mode & 0x0002) == 0x0002;
      result += GetDynamicLengths(lz77, lstart, lend, ll_lengths, d_lengths,
                                  usebrotli, revcounts, ohh);
    } else {
      ZopfliBlockState s;
      ZopfliLZ77Store store;
      ZopfliIterations iterations;
      ZopfliOptions* options2 = Zmalloc(sizeof(ZopfliOptions)); 
      unsigned iter = 0;
      size_t instart = lz77->pos[lstart];
      size_t inend = instart + ZopfliLZ77GetByteRange(lz77, lstart, lend);
      memcpy(options2,options,sizeof(ZopfliOptions));
      options2->numthreads = 0;
      options2->numiterations = 0;
      options2->verbose = 0;
      ZopfliInitLZ77Store(lz77->data, &store);
      ZopfliInitBlockState(options2, instart, inend, 1, &s);
      mui = options->slowdynmui;
      result = ZopfliLZ77Optimal(&s, lz77->data, instart, inend, &store, &iterations, NULL, &iter);
      mui = 0;
      free(options2);
      ZopfliCleanBlockState(&s);
      ZopfliCleanLZ77Store(&store);
    }
  }

  return result;
}

zfloat ZopfliCalculateBlockSizeAutoType(const ZopfliOptions* options,
                                        const ZopfliLZ77Store* lz77,
                                        size_t lstart, size_t lend, int v,
                                        int expensivedyn) {
  zfloat bestcost;
  zfloat uncompressedcost = ZopfliCalculateBlockSize(options, lz77, lstart, lend, 0, 0);
  zfloat fixedcost = ZOPFLI_LARGE_FLOAT;
  zfloat dyncost = ZopfliCalculateBlockSize(options, lz77, lstart, lend, 2, expensivedyn);
  ZopfliLZ77Store fixedstore;

  /* Don't do the expensive fixed cost calculation for larger blocks that are
     unlikely to use it.
     We allow user to enable expensive fixed calculations on all blocks */
  if (options->mode & 0x0080 || lz77->size<=1000) {
    /* Recalculate the LZ77 with ZopfliLZ77OptimalFixed */
    size_t instart = lz77->pos[lstart];
    size_t inend = instart + ZopfliLZ77GetByteRange(lz77, lstart, lend);

    ZopfliBlockState s;
    ZopfliInitLZ77Store(lz77->data, &fixedstore);
    ZopfliInitBlockState(options, instart, inend, 1, &s);
    ZopfliLZ77OptimalFixed(&s, lz77->data, instart, inend, &fixedstore);
    fixedcost = ZopfliCalculateBlockSize(options, &fixedstore, 0, fixedstore.size, 1, 0);
    ZopfliCleanBlockState(&s);
    ZopfliCleanLZ77Store(&fixedstore);
  } else {
    fixedcost = ZopfliCalculateBlockSize(options, lz77, lstart, lend, 1, 0);
  }
  if (uncompressedcost < fixedcost && uncompressedcost < dyncost) {
    bestcost = uncompressedcost;
    if(v>2) fprintf(stderr, " > Uncompressed Block is smaller:"
                            " %lu bit < %lu bit\n",(unsigned long)bestcost,(unsigned long)dyncost);
  } else if (fixedcost < dyncost) {
    bestcost = fixedcost;
    if(v>2) fprintf(stderr, " > Fixed Tree Block is smaller:"
                            " %lu bit < %lu bit\n",(unsigned long)bestcost,(unsigned long)dyncost);
  } else {
    bestcost = dyncost;
  }
  return bestcost;
}

/* Since an uncompressed block can be max 65535 in size, it actually adds
multible blocks if needed. */
static void AddNonCompressedBlock(const ZopfliOptions* options, int final,
                                  const unsigned char* in, size_t instart,
                                  size_t inend,
                                  unsigned char* bp,
                                  unsigned char** out, size_t* outsize) {
  size_t pos = instart;
  (void)options;
  for (;;) {
    size_t i;
    unsigned short blocksize = 65535;
    unsigned short nlen;
    int currentfinal;

    if (pos + blocksize > inend) blocksize = inend - pos;
    currentfinal = pos + blocksize >= inend;

    nlen = ~blocksize;

    AddBit(final && currentfinal, bp, out, outsize);
    /* BTYPE 00 */
    AddBit(0, bp, out, outsize);
    AddBit(0, bp, out, outsize);

    /* Any bits of input up to the next byte boundary are ignored. */
    *bp = 0;

    ZOPFLI_APPEND_DATA(blocksize % 256, out, outsize);
    ZOPFLI_APPEND_DATA((blocksize / 256) % 256, out, outsize);
    ZOPFLI_APPEND_DATA(nlen % 256, out, outsize);
    ZOPFLI_APPEND_DATA((nlen / 256) % 256, out, outsize);

    for (i = 0; i < blocksize; i++) {
      ZOPFLI_APPEND_DATA(in[pos + i], out, outsize);
    }

    if (currentfinal) break;
    pos += blocksize;
  }
}

/*
Adds a deflate block with the given LZ77 data to the output.
options: global program options
btype: the block type, must be 1 or 2
final: whether to set the "final" bit on this block, must be the last block
litlens: literal/length array of the LZ77 data, in the same format as in
    ZopfliLZ77Store.
dists: distance array of the LZ77 data, in the same format as in
    ZopfliLZ77Store.
lstart: where to start in the LZ77 data
lend: where to end in the LZ77 data (not inclusive)
expected_data_size: the uncompressed block size, used for assert, but you can
  set it to 0 to not do the assertion.
bp: output bit pointer
out: dynamic output array to append to
outsize: dynamic output array size
*/
static void AddLZ77Block(const ZopfliOptions* options, int btype, int final,
                         const ZopfliLZ77Store* lz77,
                         size_t lstart, size_t lend,
                         size_t expected_data_size,
                         unsigned char* bp,
                         unsigned char** out, size_t* outsize) {
  unsigned ll_lengths[ZOPFLI_NUM_LL];
  unsigned d_lengths[ZOPFLI_NUM_D];
  unsigned ll_symbols[ZOPFLI_NUM_LL];
  unsigned d_symbols[ZOPFLI_NUM_D];
  size_t detect_block_size = *outsize;
  size_t treesize = 0;
  size_t compressed_size;
  size_t uncompressed_size = 0;
  size_t i;
  if (btype == 0) {
    size_t length = ZopfliLZ77GetByteRange(lz77, lstart, lend);
    size_t pos = lstart == lend ? 0 : lz77->pos[lstart];
    size_t end = pos + length;
    AddNonCompressedBlock(options, final,
                          lz77->data, pos, end, bp, out, outsize);
    return;
  }

  AddBit(final, bp, out, outsize);
  AddBit(btype & 1, bp, out, outsize);
  AddBit((btype & 2) >> 1, bp, out, outsize);

  if (btype == 1) {
    /* Fixed block. */
    GetFixedTree(ll_lengths, d_lengths);
  } else {
    /* Dynamic block. */
    int usebrotli = (options->mode & 0x0008) == 0x0008;
    int revcounts = (options->mode & 0x0004) == 0x0004;
    int ohh = (options->mode & 0x0002) == 0x0002;
    assert(btype == 2);

    GetDynamicLengths(lz77, lstart, lend, ll_lengths, d_lengths,
                      usebrotli, revcounts, ohh);

    treesize = *outsize;
    AddDynamicTree(ll_lengths, d_lengths, bp, out, outsize,
                   ohh, revcounts);
    treesize = *outsize - treesize;
  }

  ZopfliLengthsToSymbols(ll_lengths, ZOPFLI_NUM_LL, 15, ll_symbols);
  ZopfliLengthsToSymbols(d_lengths, ZOPFLI_NUM_D, 15, d_symbols);

  AddLZ77Data(lz77, lstart, lend, expected_data_size,
              ll_symbols, ll_lengths, d_symbols, d_lengths,
              bp, out, outsize);
  /* End symbol. */
  AddHuffmanBits(ll_symbols[256], ll_lengths[256], bp, out, outsize);

  for (i = lstart; i < lend; i++) {
    uncompressed_size += lz77->dists[i] == 0 ? 1 : lz77->litlens[i];
  }
  compressed_size = *outsize - detect_block_size;
  if (options->verbose>2) PrintBlockSummary(uncompressed_size,compressed_size,treesize);
}

static void AddLZ77BlockAutoType(const ZopfliOptions* options, int final,
                                 const ZopfliLZ77Store* lz77,
                                 size_t lstart, size_t lend,
                                 size_t expected_data_size,
                                 unsigned char* bp,
                                 unsigned char** out, size_t* outsize) {
  zfloat uncompressedcost = ZopfliCalculateBlockSize(options, lz77, lstart, lend, 0, 0);
  zfloat fixedcost = ZopfliCalculateBlockSize(options, lz77, lstart, lend, 1, 0);
  zfloat dyncost = ZopfliCalculateBlockSize(options, lz77, lstart, lend, 2, 0);

  /* Whether to perform the expensive calculation of creating an optimal block
  with fixed huffman tree to check if smaller. Only do this for small blocks or
  blocks which already are pretty good with fixed huffman tree.

  Expensive fixed calculation is hardcoded ON, because unlike block splitter,
  it's rather fast here.
  */
  int expensivefixed = 1;

  ZopfliLZ77Store fixedstore;
  if (lstart == lend) {
    /* Smallest empty block is represented by fixed block */
    AddBits(final, 1, bp, out, outsize);
    AddBits(1, 2, bp, out, outsize);  /* btype 01 */
    AddBits(0, 7, bp, out, outsize);  /* end symbol has code 0000000 */
    return;
  }
  ZopfliInitLZ77Store(lz77->data, &fixedstore);
  if (expensivefixed) {
    /* Recalculate the LZ77 with ZopfliLZ77OptimalFixed */
    size_t instart = lz77->pos[lstart];
    size_t inend = instart + ZopfliLZ77GetByteRange(lz77, lstart, lend);

    ZopfliBlockState s;
    ZopfliInitBlockState(options, instart, inend, 1, &s);
    ZopfliLZ77OptimalFixed(&s, lz77->data, instart, inend, &fixedstore);
    fixedcost = ZopfliCalculateBlockSize(options, &fixedstore, 0, fixedstore.size, 1, 0);
    ZopfliCleanBlockState(&s);
  }
  if (uncompressedcost < fixedcost && uncompressedcost < dyncost) {
    AddLZ77Block(options, 0, final, lz77, lstart, lend,
                 expected_data_size, bp, out, outsize);
    if (options->verbose>2) fprintf(stderr, " > Used Uncompressed Block(s):"
                            " %lu bit < %lu bit\n",(unsigned long)uncompressedcost,(unsigned long)dyncost);
  } else if (fixedcost < dyncost) {
    if (expensivefixed) {
      AddLZ77Block(options, 1, final, &fixedstore, 0, fixedstore.size,
                   expected_data_size, bp, out, outsize);
    } else {
      AddLZ77Block(options, 1, final, lz77, lstart, lend,
                   expected_data_size, bp, out, outsize);
    }
    if (options->verbose>2) fprintf(stderr, " > Used Fixed Tree Block:"
                            " %lu bit < %lu bit\n",(unsigned long)fixedcost,(unsigned long)dyncost);
  } else {
    AddLZ77Block(options, 2, final, lz77, lstart, lend,
                 expected_data_size, bp, out, outsize);
  }

  ZopfliCleanLZ77Store(&fixedstore);
}

static void Sl(size_t* a, size_t b) {
  if(*a<b) *a=b;
}

static size_t freadst(void* buffer, unsigned char sizetsize, int dummy, FILE *stream) {
  size_t a = 0, b;
  unsigned char byte;
  (void)dummy;
  for(b = 0; b < sizetsize; ++b) {
    a += fread(&byte, 1, 1, stream);
    ((unsigned char *)buffer)[b] = byte;
  }
  for(;b < sizeof(size_t); ++b) {
    ((unsigned char *)buffer)[b] = 0;
  }
  return a;
}

static void Verifysize_t(size_t verifysize, unsigned char* sizetsize) {
  int j = sizeof(size_t) - 1;
  for(;; --j) {
    unsigned char *p = (unsigned char*)&verifysize;
    if(p[j]==0) {
      --(*sizetsize);
    } else {
      return;
    }
    if(j==0) return;
  }
}

typedef struct ZopfliBestStats {

  char mode;

  size_t blocksize;

  unsigned long blockcrc;

  unsigned int startiteration;

  SymbolStats* beststats;
} ZopfliBestStats;

static int StatsDBLoad(ZopfliBestStats* statsdb) {
  FILE *file;
  size_t b = 0, i = 0, j = 0;
  unsigned char check;
  char crc32bits[9];
  char DBfile[32];
  char LocBuf[56];
  unsigned char sizetsize = sizeof(size_t);
  sprintf(crc32bits,"%08lx",statsdb->blockcrc);
  sprintf(DBfile,"%x-%lu.dat",statsdb->mode,(unsigned long)statsdb->blocksize);
  sprintf(LocBuf,"ZopfliDB");
  while(i<(sizeof(crc32bits) / sizeof(crc32bits[0]))) {
    if((i % 2) == 0) {
        LocBuf[8+i+j] = '/';
        ++j;
    }
    LocBuf[8+i+j] = crc32bits[i];
    ++i;
  }
  sprintf(LocBuf+21,"%s",DBfile);
  file = fopen(LocBuf, "rb");
  if(!file) return 0;
  if(statsdb->beststats == NULL) {
    statsdb->beststats = Zmalloc(sizeof(SymbolStats));
    InitStats(statsdb->beststats);
  }
  b += fread(&check,sizeof(check),1,file);
  if(check != BESTSTATSDBVER) return 0;
  b += fread(&check,sizeof(check),1,file);
  if(check != sizeof(zfloat)) return 0;
  b += fread(&sizetsize, sizeof(sizetsize),1,file);
  b += fread(&statsdb->startiteration, sizeof(statsdb->startiteration), 1, file);
  for(i = 0; i < ZOPFLI_NUM_LL; ++i)
    b += freadst(&statsdb->beststats->litlens[i], sizetsize, 1, file);
  for(i = 0; i < ZOPFLI_NUM_D; ++i)
    b += freadst(&statsdb->beststats->dists[i], sizetsize, 1, file);
  for(i = 0; i < ZOPFLI_NUM_LL; ++i)
    b += fread(&statsdb->beststats->ll_symbols[i], sizeof(zfloat), 1, file);
  for(i = 0; i < ZOPFLI_NUM_D; ++i)
    b += fread(&statsdb->beststats->d_symbols[i], sizeof(zfloat), 1, file);
  fclose(file);
  return 1;
}

static int DoDir(char* dir) {
  DIR* testdir;
  testdir = opendir(dir);
  if(testdir) {
    closedir(testdir);
  } else if (ENOENT == errno) {
#ifdef _WIN32
    mkdir(dir);
#else
    mkdir(dir, 0777);
#endif
  } else {
    return 0;
  }
  return 1;
}

static int StatsDBSave(ZopfliBestStats* statsdb) {
  FILE *file;
  size_t b = 0, i = 0, j = 0;
  unsigned char check = BESTSTATSDBVER;
  char crc32bits[9];
  char DBfile[32];
  char LocBuf[56];
  unsigned char sizetsize = sizeof(size_t);
  size_t verifysize = 0;
  if(statsdb->beststats == NULL) return 0;
  sprintf(crc32bits,"%08lx",statsdb->blockcrc);
  sprintf(DBfile,"%x-%lu.dat",statsdb->mode,(unsigned long)statsdb->blocksize);
  sprintf(LocBuf,"ZopfliDB");
  while(i<(sizeof(crc32bits) / sizeof(crc32bits[0]))) {
    if((i % 2) == 0) {
        LocBuf[8+i+j] = 0;
        if(!DoDir(LocBuf)) return 0;
        LocBuf[8+i+j] = '/';
        ++j;
    }
    LocBuf[8+i+j] = crc32bits[i];
    ++i;
  }
  sprintf(LocBuf+21,"%s",DBfile);
  file = fopen(LocBuf, "wb");
  if(!file) return 0;
  for(i = 0; i < ZOPFLI_NUM_LL; ++i)
    Sl(&verifysize,statsdb->beststats->litlens[i]);
  for(i = 0; i < ZOPFLI_NUM_D; ++i)
    Sl(&verifysize,statsdb->beststats->dists[i]);
  Verifysize_t(verifysize, &sizetsize);
  b += fwrite(&check, sizeof(check), 1, file);
  check = sizeof(zfloat);
  b += fwrite(&check, sizeof(check), 1, file);
  b += fwrite(&sizetsize, sizeof(sizetsize), 1, file);
  b += fwrite(&statsdb->startiteration, sizeof(statsdb->startiteration), 1, file);
  for(i = 0; i < ZOPFLI_NUM_LL; ++i)
    b += fwrite(&statsdb->beststats->litlens[i], sizetsize, 1, file);
  for(i = 0; i < ZOPFLI_NUM_D; ++i)
    b += fwrite(&statsdb->beststats->dists[i], sizetsize, 1, file);
  for(i = 0; i < ZOPFLI_NUM_LL; ++i)
    b += fwrite(&statsdb->beststats->ll_symbols[i], sizeof(zfloat), 1, file);
  for(i = 0; i < ZOPFLI_NUM_D; ++i)
    b += fwrite(&statsdb->beststats->d_symbols[i], sizeof(zfloat), 1, file);
  fclose(file);
  return 1;
}

static void PrintProgress(int v, size_t start, size_t inend, size_t i, size_t n, size_t npoints) {
  if(v>0) fprintf(stderr, "Progress: %5.1f%%",100.0 * (zpfloat) start / (zpfloat)inend);
  if(v>1) {
    char buff[24];
    char buff2[2];
    size_t j;
    unsigned long dleft = inend - start;
    buff2[1] =  0 ;
    if(dleft > 10238976) {
      dleft /= 1048576;
      buff2[0] = 'M';
    } else if(dleft > 99999) {
      dleft /= 1024;
      buff2[0] = 'K';
    } else {
      buff2[0] = ' ';
    }
    sprintf(buff,"%5lu",dleft);
    for(j = 0; buff[j] != 0; ++j) {}
    sprintf(buff+j,"%sB",buff2);
    fprintf(stderr, "  ---  Block: %4lu / %lu [%04lu]  ---  Data Left: %s          ",
            (unsigned long)i, (unsigned long)(npoints + 1), (unsigned long)(n + 1), buff);
    if(v>2) {
      fprintf(stderr,"\n");
    } else {
      fprintf(stderr,"  \r");
    }
  } else {
    fprintf(stderr,"\r");
  }
}

typedef struct ZopfliThread {
  const ZopfliOptions* options;

  int is_running;

  int is_started;

  size_t start;

  size_t end;

  size_t statspos;

  const unsigned char* in;

  zfloat cost;

  int bestperblock;

  int allstatscontrol;
  
  int mode;

  unsigned int startiteration;

  ZopfliIterations iterations;

  ZopfliLZ77Store store;

  SymbolStats* beststats;

  size_t affmask;
} ZopfliThread;

#ifdef _WIN32
DWORD WINAPI threading(void *a) {
#else
static void *threading(void *a) {
#endif

  ZopfliThread *b = (ZopfliThread *)a;

  while(b->is_running == 1) {
    int tries = 1;
    size_t blocksize = 0;
    unsigned long blockcrc = 0;
    ZopfliLZ77Store store;
    ZopfliInitLZ77Store(b->in, &b->store);

    if(b->options->mode & 0x0010) {
      tries=16;
      if(b->options->numthreads == 0 && (b->options->mode & 0x0100)) {
        blocksize = b->end - b->start;
        blockcrc = CRC(b->in + b->start, blocksize);
      }
    }
    do {
      zfloat tempcost;
      ZopfliBlockState s;
      ZopfliOptions o = *(b->options);
      ZopfliInitLZ77Store(b->in, &store);
      --tries;
      if(b->options->mode & 0x0010) {
        b->mode = tries;
        o.mode = tries + (o.mode & 0xFFF0);
        b->startiteration = 0;
        if(b->options->mode & 0x0100) {
          if(b->options->numthreads > 0) {
            /* Racing condition prevention */
            b->allstatscontrol = tries + 0x0100;
            do {
              usleep(50000);
            } while(b->allstatscontrol & 0x0100);
          } else {
            /* No SLAVE threads, work done by MASTER thread */
            ZopfliBestStats statsdb;
            statsdb.blocksize = blocksize;
            statsdb.blockcrc = blockcrc;
            statsdb.mode = tries;
            statsdb.beststats = 0;
            if(StatsDBLoad(&statsdb)) {
              b->beststats = statsdb.beststats;
              b->startiteration = statsdb.startiteration;
            }
          }
        }
      } else {
        b->mode = (o.mode & 0xF);
      }

      ZopfliInitBlockState(&o, b->start, b->end, 1, &s);

      ZopfliLZ77Optimal(&s, b->in, b->start, b->end, &store, &b->iterations,
                      (b->options->mode & 0x0100)?&b->beststats:NULL, &b->startiteration);
      tempcost = ZopfliCalculateBlockSizeAutoType(&o, &store, 0, store.size, 2, 0);

      ZopfliCleanBlockState(&s);

      if(b->cost==0 || tempcost<b->cost) {
        ZopfliCleanLZ77Store(&b->store);
        ZopfliInitLZ77Store(b->in, &b->store);
        ZopfliCopyLZ77Store(&store,&b->store);
        b->bestperblock = o.mode;
        if(b->options->verbose == 6) {
          fprintf(stderr,"      [BLK: %d | MODE: %s%s%s%s] Best: %lu bit",
                  ((unsigned int)b->iterations.block+1),
                  (b->mode & 0x8)? "1" : "0",
                  (b->mode & 0x4)? "1" : "0",
                  (b->mode & 0x2)? "1" : "0",
                  (b->mode & 0x1)? "1" : "0",
                  (unsigned long)tempcost);
          if(b->cost!=0) {
              fprintf(stderr," < %lu bit",(unsigned long)b->cost);
          }
          fprintf(stderr,"             \n");
        }
        b->cost = tempcost;
      }
      ZopfliCleanLZ77Store(&store);

      if((b->options->mode & 0x0110) == 0x0110) {
        if(b->options->numthreads > 0) {
          /* Racing condition prevention */
          b->allstatscontrol = tries + 0x0200;
          do {
            usleep(50000);
          } while(b->allstatscontrol & 0x0200);
        } else {
          /* No SLAVE threads, work done by MASTER thread */
          ZopfliBestStats statsdb;
          statsdb.blocksize = blocksize;
          statsdb.blockcrc = blockcrc;
          statsdb.mode = tries;
          statsdb.beststats = b->beststats;
          statsdb.startiteration = b->startiteration;
          StatsDBSave(&statsdb);
          FreeStats(statsdb.beststats);
          free(statsdb.beststats);
          b->beststats = 0;
        }
      }

    } while(tries>0);

    b->is_running = 2;
    while(b->is_running != 1 && b->is_running != 3 && b->options->numthreads) {
      usleep(50000);
    }
    
  }

  if(b->options->numthreads) b->is_running = -1;

  return 0;

}

typedef struct ZopfliBlockInfo {

  size_t pos;

  size_t start;

  size_t end;

  size_t len;
  
  unsigned long crc;
} ZopfliBlockInfo;

static void ZopfliUseThreads(const ZopfliOptions* options,
                               ZopfliLZ77Store* lz77,
                               const unsigned char* in,
                               size_t instart, size_t inend,
                               size_t bkstart, size_t bkend,
                               size_t** splitpoints,
                               size_t** splitpoints_uncompressed,
                               int** bestperblock,
                               zfloat *totalcost, int v) {
  unsigned showcntr = 4;
  unsigned showthread = 0;
  unsigned threadsrunning = 0;
  unsigned threnum = 0;
  unsigned numthreads = options->numthreads>0?options->numthreads>bkend+1?bkend+1:options->numthreads:1;
  int neednext = 0;
  size_t nextblock = bkstart;
  size_t n, i;
#ifndef _WIN32
  cpu_set_t *cpuset = Zmalloc(sizeof(cpu_set_t) * options->affamount);
#endif
  zfloat *tempcost = Zmalloc(sizeof(*tempcost) * (bkend+1));
  unsigned char nomoredata = 0;
  unsigned char* blockdone = Zcalloc(bkend+1,sizeof(unsigned char));
#ifdef _WIN32
  HANDLE *thr = Zmalloc(sizeof(HANDLE) * numthreads);
#else
  pthread_t *thr = Zmalloc(sizeof(pthread_t) * numthreads);
  pthread_attr_t *thr_attr = Zmalloc(sizeof(pthread_attr_t) * numthreads);
#endif
  ZopfliThread *t = Zmalloc(sizeof(ZopfliThread) * numthreads);
  ZopfliLZ77Store *tempstore = Zmalloc(sizeof(ZopfliLZ77Store) * (bkend+1));
  ZopfliBestStats* statsdb = Zmalloc(sizeof(ZopfliBestStats) * numthreads);

  ZopfliBlockInfo *blockinfo     = Zmalloc(sizeof(ZopfliBlockInfo) * (bkend+1));
  ZopfliBlockInfo *tempblockinfo = Zmalloc(sizeof(ZopfliBlockInfo));
  size_t processedbytes = instart;
  size_t processedblocks = 0;

#ifndef _WIN32
  for(i=0;i<options->affamount;++i) {
    CPU_ZERO(&(cpuset[i]));
    {
      size_t cntr = 0;
      size_t bitpos = 1;
      while(bitpos <= options->threadaffinity[i]) {
        if(options->threadaffinity[i] && bitpos)
          CPU_SET(cntr, &(cpuset[i]));
        bitpos = bitpos << 1;
        ++cntr;
      }
    }
  }
#endif
  {
    size_t affcntr = 0;
    for(i=0;i<numthreads;++i) {
#ifndef _WIN32
     pthread_attr_init(&(thr_attr[i]));
     pthread_attr_setdetachstate(&(thr_attr[i]), PTHREAD_CREATE_DETACHED);
#endif
     t[i].is_started = 0;
     t[i].is_running = 0;
     t[i].allstatscontrol = 0;
     statsdb[i].beststats = 0;
     if(options->affamount>0) {
#ifdef _WIN32
       t[i].affmask = options->threadaffinity[affcntr];
#else
       t[i].affmask = affcntr;
#endif
       ++affcntr;
       if(affcntr >= options->affamount) affcntr = 0;
     }
    }
  }

  for (i = bkstart; i <= bkend; ++i) {
    blockinfo[i].pos   = i;
    blockinfo[i].start = i == 0 ? instart : (*splitpoints_uncompressed)[i - 1];
    blockinfo[i].end   = i == bkend ? inend : (*splitpoints_uncompressed)[i];
    blockinfo[i].len   = blockinfo[i].end - blockinfo[i].start;
    if((options->mode & 0x0100) && !(options->numthreads == 0 && (options->mode & 0x0010))) {
      blockinfo[i].crc = CRC(in + blockinfo[i].start, blockinfo[i].len);
    } else {
      blockinfo[i].crc = 0;
    }
  }


  for(i = bkstart+1; i <= bkend; ++i) {
    for(n = bkstart; n <= bkend - i; ++n) {
      if(blockinfo[n].len < blockinfo[n + 1].len) {
        *tempblockinfo   = blockinfo[n];
        blockinfo[n]     = blockinfo[n + 1];
        blockinfo[n + 1] = *tempblockinfo;
      }
    }
  }
  free(tempblockinfo);


  for (i = bkstart; i <= bkend; ++i) {
    do {
      neednext=0;
      for(;threnum<numthreads;) {
        if(t[threnum].is_running==1) {
          if(options->mode & 0x0010) {
              size_t xx = 0;
              for(;xx < numthreads;++xx) {
                  if(t[xx].allstatscontrol & 0x0100) {
                    statsdb[xx].beststats = 0;
                    statsdb[xx].mode = t[xx].allstatscontrol & 0xF;
                    if(StatsDBLoad(&statsdb[xx])) {
                      t[xx].beststats = statsdb[xx].beststats;
                      t[xx].startiteration = statsdb[xx].startiteration;
                    }
                    t[xx].allstatscontrol = 0;
                  } else if(t[xx].allstatscontrol & 0x0200 && t[xx].beststats != 0) {
                    statsdb[xx].beststats = t[xx].beststats;
                    statsdb[xx].startiteration = t[xx].startiteration;
                    StatsDBSave(&statsdb[xx]);
                    FreeStats(statsdb[xx].beststats);
                    free(statsdb[xx].beststats);
                    statsdb[xx].beststats = 0;
                    t[xx].beststats = 0;
                    t[xx].allstatscontrol = 0;
                  }
              }
          }
          if(mui)
            t[threnum].iterations.maxfailiterations = mui;
          if(options->verbose>2) {
            if(t[showthread].is_running==1) {
              unsigned calci, thrprogress;
              if(mui==0) {
                calci = options->numiterations;
              } else {
                calci = (unsigned)(t[showthread].iterations.bestiteration+mui);
                if(calci>options->numiterations) calci=options->numiterations;
              }
              thrprogress = (int)(((zfloat)t[showthread].iterations.iteration / (zfloat)calci) * 100);
              usleep(125000);
              fprintf(stderr,"%3d%% T:%2d | B:%4d | M:%s%s%s%s | I:%5d (%d) - %d (%d) b      \r",
                      thrprogress, showthread, ((unsigned int)t[showthread].iterations.block+1),
                      (t[showthread].mode & 0x8)? "1" : "0",
                      (t[showthread].mode & 0x4)? "1" : "0",
                      (t[showthread].mode & 0x2)? "1" : "0",
                      (t[showthread].mode & 0x1)? "1" : "0",
                      t[showthread].iterations.iteration, t[showthread].iterations.bestiteration,
                      t[showthread].iterations.cost, t[showthread].iterations.bestcost);
            } else {
              ++showthread;
              if(showthread>=numthreads)
                showthread=0;
              showcntr=0;
            }
            if(showcntr>8) {
              if(threadsrunning>1) {
                ++showthread;
                if(showthread>=numthreads)
                  showthread=0;
              }
              showcntr=1;
            } else {
              ++showcntr;
            }
            ++threnum;
            if(threnum>=numthreads)
              threnum=0;
          } else {
            usleep(50000);
            ++threnum;
            if(threnum>=numthreads)
              threnum=0;
          }
        }
        if(t[threnum].is_running==0) {
          if(nomoredata == 0) {
            t[threnum].beststats = 0;
            t[threnum].startiteration = 0;
            if(options->mode & 0x0100) {
              statsdb[threnum].blocksize = blockinfo[i].len;
              statsdb[threnum].blockcrc = blockinfo[i].crc;
              if(!(options->mode & 0x0010)) {
                statsdb[threnum].mode = options->mode & 0xF;
                if(StatsDBLoad(&statsdb[threnum])) {
                  t[threnum].beststats = statsdb[threnum].beststats;
                  t[threnum].startiteration = statsdb[threnum].startiteration;
                }
              }
            }            
            t[threnum].options = options;
            t[threnum].start = blockinfo[i].start;
            t[threnum].end = blockinfo[i].end;
            t[threnum].in = in;
            t[threnum].cost = 0;
            t[threnum].allstatscontrol = 0;
            t[threnum].mode = 0;
            t[threnum].iterations.block = blockinfo[i].pos;
            t[threnum].iterations.bestcost = 0;
            t[threnum].iterations.cost = 0;
            t[threnum].iterations.iteration = 0;
            t[threnum].iterations.bestiteration = 0;
            t[threnum].iterations.maxfailiterations = (mui?mui:options->maxfailiterations);
            t[threnum].is_running = 1;
            if(options->numthreads) {
              if(t[threnum].is_started == 0) {
                t[threnum].is_started = 1;
#ifdef _WIN32
                thr[threnum] = CreateThread(NULL, 2097152, threading, (void *)&t[threnum], 0, NULL);
#else
                pthread_create(&thr[threnum], &(thr_attr[threnum]), threading, (void *)&t[threnum]);
#endif
                if(options->affamount>0) {
#ifdef _WIN32
                  SetThreadAffinityMask(thr[threnum], t[threnum].affmask);
#else
                  pthread_setaffinity_np(thr[threnum], sizeof(cpu_set_t), &cpuset[t[threnum].affmask]);
#endif
                }
              }
            } else {
#ifdef _WIN32
              threading(&t[threnum]);
#else
              (*threading)(&t[threnum]);
#endif
            }
            ++threadsrunning;
            if(i>=bkend)
              nomoredata = 1;
            else
              neednext=1;
          }
          ++threnum;
          if(threnum>=numthreads)
            threnum=0;
        }
        if(t[threnum].is_running==2) {
          processedbytes += t[threnum].end - t[threnum].start;
          ++processedblocks;
          PrintProgress(v, processedbytes, inend, processedblocks, t[threnum].iterations.block, bkend);
          if(options->mode & 0x0010) {
            (*bestperblock)[t[threnum].iterations.block] = t[threnum].bestperblock;
          }
          if(options->mode & 0x0100 && t[threnum].beststats != 0) {
            if(!(options->mode & 0x0010)) {
              statsdb[threnum].beststats = t[threnum].beststats;
              statsdb[threnum].startiteration = t[threnum].startiteration;
              StatsDBSave(&statsdb[threnum]);
              FreeStats(statsdb[threnum].beststats);
              free(statsdb[threnum].beststats);
            }
          }
          statsdb[threnum].beststats = 0;
          t[threnum].beststats = 0;
          if(nextblock==t[threnum].iterations.block) {
            size_t realloc_size = 0;
            *totalcost += t[threnum].cost;
            ZopfliAppendLZ77Store(&t[threnum].store, lz77);
            ZopfliCleanLZ77Store(&t[threnum].store);
            if(t[threnum].iterations.block < bkend) (*splitpoints)[t[threnum].iterations.block] = lz77->size;
            for(n=(nextblock+1);n<=i;++n) {
              if(blockdone[n]==0) break;
              realloc_size += (&tempstore[n])->size;
            }
            if(realloc_size >= ZOPFLI_REALLOC_BUFFER)
              ZopfliReallocLZ77Store(lz77->size + realloc_size, lz77);
            for(n=(nextblock+1);n<=i;++n) {
              if(blockdone[n]==0) break;
              ZopfliAppendLZ77Store(&tempstore[n], lz77);
              ZopfliCleanLZ77Store(&tempstore[n]);
              if(n < bkend) (*splitpoints)[n] = lz77->size;
              *totalcost += tempcost[n];
              blockdone[n]=0;
              ++nextblock;
              if(options->verbose>2)
                fprintf(stderr,"STREAM <= BLOCK: %4d / %4d [%4d]     \r",(unsigned int)(n + 1),(unsigned int)(i + 1),(unsigned int)(bkend + 1));
            }
            ++nextblock;
          } else {
            ZopfliInitLZ77Store(in,&tempstore[(t[threnum].iterations.block)]);
            ZopfliCopyLZ77Store(&t[threnum].store, &tempstore[(t[threnum].iterations.block)]);
            ZopfliCleanLZ77Store(&t[threnum].store);
            tempcost[(t[threnum].iterations.block)] = t[threnum].cost;
            blockdone[(t[threnum].iterations.block)] = 1;
          }
          t[threnum].is_running=0;
          --threadsrunning;
          ++threnum;
          if(threnum>=numthreads) threnum=0;
          if(threadsrunning==0 &&
            (neednext==1 || nomoredata==1)) break;
          if(nomoredata==1 && threadsrunning<=options->numthreadsstop) {
            for(n=0;n<numthreads;++n) {
              t[n].iterations.maxfailiterations = 1;
            }
          }
        }
        if(neednext==1) break;
      } 
    } while(threadsrunning>0 && neednext==0);
  }

  if(options->numthreads) {
    size_t x = 0;
    for(;x<numthreads;++x) {
      t[x].is_running = 3;
      while(t[x].is_running != -1)
        usleep(50000);
#ifdef _WIN32
      WaitForSingleObject(thr[x], INFINITE);
      CloseHandle(thr[x]);
#endif
    }
  }

  free(blockinfo);
  free(statsdb);
  free(blockdone);
  free(tempstore);
  free(t);
  free(thr);
#ifndef _WIN32
  free(thr_attr);
  free(cpuset);
#endif
  free(tempcost);
}

/*
Deflate a part, to allow ZopfliDeflate() to use multiple master blocks if
needed.
It is possible to call this function multiple times in a row, shifting
instart and inend to next bytes of the data. If instart is larger than 0, then
previous bytes are used as the initial dictionary for LZ77.
This function will usually output multiple deflate blocks. If final is 1, then
the final bit will be set on the last block.

This function can parse custom block split points and do additional splits
inbetween if necessary. So it's up to You if to relly on built-in block splitter
or for example use KZIP block split points etc.
Original split points will be overwritten inside ZopfliPredefinedSplits
structure (sp) with the best ones that Zopfli found.
ZopfliPredefinedSplits can be safely passed as NULL pointer to disable
this functionality.
*/
DLL_PUBLIC void ZopfliDeflatePart(const ZopfliOptions* options, int btype, int final,
                          const unsigned char* in, size_t instart, size_t inend,
                          unsigned char* bp, unsigned char** out,
                          size_t* outsize, int v, ZopfliPredefinedSplits *sp) {
  size_t i;
  /* byte coordinates rather than lz77 index */
  size_t* splitpoints_uncompressed = 0;
  size_t npoints = 0;
  size_t* splitpoints = 0;
  zfloat totalcost = 0;
  int pass = 0;
  zfloat alltimebest = 0;
  int* bestperblock = 0;
  int* bestperblock2 = 0;
  ZopfliLZ77Store lz77;

  /* If btype=2 is specified, it tries all block types. If a lesser btype is
  given, then however it forces that one. Neither of the lesser types needs
  block splitting as they have no dynamic huffman trees. */
  if (btype == 0) {
    AddNonCompressedBlock(options, final, in, instart, inend, bp, out, outsize);
    return;
  } else if (btype == 1) {
    ZopfliLZ77Store store;
    ZopfliBlockState s;
    ZopfliInitLZ77Store(in, &store);
    ZopfliInitBlockState(options, instart, inend, 1, &s);

    ZopfliLZ77OptimalFixed(&s, in, instart, inend, &store);
    AddLZ77Block(options, btype, final, &store, 0, store.size, 0,
                 bp, out, outsize);

    ZopfliCleanBlockState(&s);
    ZopfliCleanLZ77Store(&store);
    return;
  }

  ZopfliInitLZ77Store(in, &lz77);

  if (options->blocksplitting) {
    if(sp==NULL || sp->splitpoints==NULL) {
      ZopfliBlockSplit(options, in, instart, inend,
                       options->blocksplittingmax,
                       &splitpoints_uncompressed, &npoints);
    } else {
      size_t lastknownsplit = 0;
      size_t* splitunctemp = 0;
      size_t npointstemp = 0;
      for(i = 0; i < sp->npoints; ++i) {
        if(sp->splitpoints[i] > instart && sp->splitpoints[i] < inend) {
          if(sp->moresplitting == 1) {
            size_t start = i == 0 ? instart : sp->splitpoints[i - 1];
            if(start < instart) start = instart;
            lastknownsplit = i;
            ZopfliBlockSplit(options, in, start, sp->splitpoints[i], 
                             options->blocksplittingmax,
                             &splitunctemp, &npointstemp);
            if(npointstemp > 0) {
              size_t j = 0;
              for(;j < npointstemp; ++j) {
                ZOPFLI_APPEND_DATA(splitunctemp[j], &splitpoints_uncompressed, &npoints);
              }
            }
            free(splitunctemp);
            splitunctemp = 0;
          }
          ZOPFLI_APPEND_DATA(sp->splitpoints[i], &splitpoints_uncompressed, &npoints);
        }
      }
      if(sp->moresplitting == 1) {
        ZopfliBlockSplit(options, in, sp->splitpoints[lastknownsplit] , inend,
                         options->blocksplittingmax, &splitunctemp, &npointstemp);
        if(npointstemp > 0) {
          for(i = 0; i < npointstemp; ++i) {
            ZOPFLI_APPEND_DATA(splitunctemp[i], &splitpoints_uncompressed, &npoints);
          }
        }
        free(splitunctemp);
        splitunctemp = 0;
      }
    }
    splitpoints = Zcalloc(npoints, sizeof(*splitpoints));
  }

  if(options->mode & 0x0010) {
    bestperblock = Zmalloc(sizeof(*bestperblock) * (npoints + 1));
  }

  i = 0;
  ZopfliUseThreads(options, &lz77, in, instart, inend, i, npoints,
                   &splitpoints, &splitpoints_uncompressed, &bestperblock,
                   &totalcost,v);

  alltimebest = totalcost;

  /* Second/nth block splitting attempt and optional recompression */
  if (options->blocksplitting && npoints > 0 && (options->mode & 0x0040) == 0) {
    size_t* splitpoints2;
    size_t npoints2;
    zfloat totalcost2;
    do {
      splitpoints2 = 0;
      npoints2 = 0;
      totalcost2 = 0;

      ZopfliBlockSplitLZ77(options, &lz77,
                           options->blocksplittingmax, &splitpoints2,
                           &npoints2);

      for (i = 0; i <= npoints2; i++) {
        size_t start = i == 0 ? 0 : splitpoints2[i - 1];
        size_t end = i == npoints2 ? lz77.size : splitpoints2[i];
        totalcost2 += ZopfliCalculateBlockSizeAutoType(options, &lz77, start, end, 0, 0);
      }

      ++pass;
      if(pass <= options->pass) {
        size_t* splitpoints_uncompressed2 = 0;
        size_t j = 0;
        ZopfliLZ77Store lz77temp;
        totalcost = 0;
        ZopfliInitLZ77Store(in, &lz77temp);

        if(npoints2 > 0 && splitpoints_uncompressed2==0) {
          size_t npointstemp = 0;
          size_t postemp = instart;
          for (i = 0; i < lz77.size; ++i) {
            size_t length = lz77.dists[i] == 0 ? 1 : lz77.litlens[i];
            if (splitpoints2[npointstemp] == i) {
              ZOPFLI_APPEND_DATA(postemp, &splitpoints_uncompressed2, &npointstemp);
              if (npointstemp == npoints2) break;
            }
            postemp += length;
          }
          assert(npointstemp == npoints2);
        }

        if (v>2) fprintf(stderr," Recompressing, pass #%d.\n",pass);

        if(options->mode & 0x0010) {
          bestperblock2 = Zmalloc(sizeof(*bestperblock2) * (npoints2+1));
        }

        ZopfliUseThreads(options, &lz77temp, in, instart, inend, j, npoints2,
                         &splitpoints2, &splitpoints_uncompressed2, &bestperblock2,
                         &totalcost,v);

        if (v>2) fprintf(stderr,"!! RECOMPRESS: ");
        if(totalcost < alltimebest) {
          if (v>2) fprintf(stderr,"Smaller (%lu bit < %lu bit) !\n",(unsigned long)totalcost,(unsigned long)alltimebest);
          alltimebest = totalcost;
          ZopfliCopyLZ77Store(&lz77temp,&lz77);
          ZopfliCleanLZ77Store(&lz77temp);
          free(splitpoints);
          free(splitpoints_uncompressed);
          splitpoints = splitpoints2;
          splitpoints_uncompressed = splitpoints_uncompressed2;
          free(bestperblock);
          npoints = npoints2;
          if(options->mode & 0x0010) {
            bestperblock = Zmalloc(sizeof(*bestperblock) * (npoints+1));
            for(i = 0; i<= npoints; ++i) {
              bestperblock[i] = bestperblock2[i];
            }
            free(bestperblock2);
          }
        } else {
          free(splitpoints2);
          splitpoints2=0;
          free(splitpoints_uncompressed2);
          splitpoints_uncompressed2=0;
          ZopfliCleanLZ77Store(&lz77temp);
          free(bestperblock2);
          if (v>2) fprintf(stderr,"Bigger, using last (%lu bit > %lu bit) !\n",(unsigned long)totalcost,(unsigned long)alltimebest);
          break;
        }
      } else {
        if(totalcost2 < alltimebest) {
          free(splitpoints);
          free(bestperblock);
          splitpoints = splitpoints2;
          npoints = npoints2;
          if(npoints2 > 0) {
            size_t postemp = 0;
            size_t npointstemp = 0;
            free(splitpoints_uncompressed);
            splitpoints_uncompressed = 0;
            for (i = 0; i < lz77.size; ++i) {
              size_t length = lz77.dists[i] == 0 ? 1 : lz77.litlens[i];
              if (splitpoints[npointstemp] == i) {
                ZOPFLI_APPEND_DATA(postemp, &splitpoints_uncompressed, &npointstemp);
                if (npointstemp == npoints) break;
              }
              postemp += length;
            }
            assert(npointstemp == npoints);
          }
        } else {
          free(splitpoints2);
          splitpoints2=0;
        }
      }
    } while(pass<options->pass);
  }

  for (i = 0; i <= npoints; i++) {
    size_t start = i == 0 ? 0 : splitpoints[i - 1];
    size_t end = i == npoints ? lz77.size : splitpoints[i];
    ZopfliOptions o = *options;
    if(v>2) {
      fprintf(stderr,"BLOCK %04d: ",(int)(i+1));
      if(bestperblock!=NULL) {
        fprintf(stderr,"[ LAZY: %-3s | OHH: %-3s | RC: %-3s | BROTLI: %-3s ]\n            ",
                  (bestperblock[i] & 1) == 1?"ON":"OFF",
                  (bestperblock[i] & 2) == 2?"ON":"OFF",
                  (bestperblock[i] & 4) == 4?"ON":"OFF",
                  (bestperblock[i] & 8) == 8?"ON":"OFF");
      }
    }
    if(bestperblock!=NULL) {
      o.mode = bestperblock[i] + (o.mode & 0xFFF0);
    }
    AddLZ77BlockAutoType(&o, i == npoints && final,
                         &lz77, start, end, 0,
                         bp, out, outsize);
  }

  if(npoints>0) {
    int hadsplits = 0;
    if(sp!=NULL) {
      free(sp->splitpoints);
      sp->splitpoints = 0;
      sp->npoints = 0;
      hadsplits = 1;
    }
    if(v>2) fprintf(stderr,"!! BEST SPLIT POINTS FOUND: ");
    for (i = 0; i < npoints; ++i) {
      if(hadsplits==1) {
        ZOPFLI_APPEND_DATA(splitpoints_uncompressed[i],
                           &sp->splitpoints, &sp->npoints);
      }
      if(v>2) fprintf(stderr, "%lu ", (unsigned long)(splitpoints_uncompressed[i]));
    }
    if(v>2) {
      fprintf(stderr, "(hex:");
      for (i = 0; i < npoints; ++i) {
        if(i==0) fprintf(stderr," "); else fprintf(stderr,",");
        fprintf(stderr, "%x", (int)(splitpoints_uncompressed[i]));
      }
      fprintf(stderr,")\n");
    }
  }

  ZopfliCleanLZ77Store(&lz77);
  free(splitpoints);
  free(splitpoints_uncompressed);
  free(bestperblock);
}

/*
Pretty much as the original but ensures that ZopfliPredefinedSplits
structure passes/returns proper split points when input requires
splitting to ZOPFLI_MASTER_BLOCK_SIZE chunks.
*/
DLL_PUBLIC void ZopfliDeflate(const ZopfliOptions* options, int btype, int final,
                   const unsigned char* in, size_t insize,
                   unsigned char* bp, unsigned char** out, size_t* outsize,
                   ZopfliPredefinedSplits *sp) {
  size_t offset = *outsize;
#if ZOPFLI_MASTER_BLOCK_SIZE == 0
  ZopfliDeflatePart(options, btype, final, in, 0, insize, bp, out, outsize, options->verbose, sp);
#else
  size_t i = 0;
  size_t last_npoint = 0;
  ZopfliPredefinedSplits* originalsp = Zmalloc(sizeof(ZopfliPredefinedSplits));
  ZopfliPredefinedSplits* finalsp = Zmalloc(sizeof(ZopfliPredefinedSplits));
  if(sp != NULL) {
    originalsp->splitpoints = 0;
    originalsp->npoints = 0;
    finalsp->splitpoints = 0;
    finalsp->npoints = 0;
    originalsp->moresplitting = sp->moresplitting;
    finalsp->moresplitting = sp->moresplitting;
    for(; i < sp->npoints; ++i) {
      ZOPFLI_APPEND_DATA(sp->splitpoints[i], &originalsp->splitpoints, &originalsp->npoints);
    }
    i = 0;
  }
  while (i < insize) {
    int masterfinal;
    int final2;
    size_t size;
    size_t MasterBlockSize = ZOPFLI_MASTER_BLOCK_SIZE;
    if(sp != NULL) {
      for(; last_npoint < sp->npoints; ++last_npoint) {
        if((sp->splitpoints[last_npoint] - i) > ZOPFLI_MASTER_BLOCK_SIZE) {
          MasterBlockSize = sp->splitpoints[last_npoint] - i;
          break;
        }
      }
    }
    masterfinal = (i + MasterBlockSize >= insize);
    final2 = final && masterfinal;
    size = masterfinal ? insize - i : MasterBlockSize;
    ZopfliDeflatePart(options, btype, final2,
                      in, i, i + size, bp, out, outsize, options->verbose, sp);
    if(sp != NULL) {
      size_t j = 0;
      for(; j < sp->npoints; ++j) {
        ZOPFLI_APPEND_DATA(i + sp->splitpoints[j], &finalsp->splitpoints, &finalsp->npoints);
      }
      free(sp->splitpoints);
      sp->splitpoints = 0;
      sp->npoints = 0;
      for(j = 0; j < originalsp->npoints; ++j) {
        ZOPFLI_APPEND_DATA(originalsp->splitpoints[j], &sp->splitpoints, &sp->npoints);
      }
    }
    i += size;
  }
  if(sp != NULL) {
    size_t j = 0;
    free(originalsp->splitpoints);
    free(sp->splitpoints);
    sp->splitpoints = 0;
    sp->npoints = 0;
    for(; j < finalsp->npoints; ++j) {
      ZOPFLI_APPEND_DATA(finalsp->splitpoints[j], &sp->splitpoints, &sp->npoints);
    }
    free(finalsp->splitpoints);
  }
  free(finalsp);
  free(originalsp);
#endif
  if(options->verbose>1) PrintSummary(insize,0,*outsize-offset);
}
