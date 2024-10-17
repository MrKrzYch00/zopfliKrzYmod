/*
Copyright 2011 Google Inc. All Rights Reserved.
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
#include "squeeze.h"

#include <assert.h>
#include <stdio.h>
#include <stdint.h>

#include "inthandler.h"
#include "blocksplitter.h"
#include "deflate.h"
#include "symbols.h"
#include "tree.h"
#include "util.h"

/* Sets everything to 0. */
void InitStats(SymbolStats* stats) {
  stats->litlens = Zcalloc(ZOPFLI_NUM_LL, sizeof(*stats->litlens));
  stats->dists = Zcalloc(ZOPFLI_NUM_D, sizeof(*stats->dists));

  stats->ll_symbols = Zcalloc(ZOPFLI_NUM_LL, sizeof(*stats->ll_symbols));
  stats->d_symbols = Zcalloc(ZOPFLI_NUM_D, sizeof(*stats->d_symbols));
}

void CopyStats(SymbolStats* source, SymbolStats* dest) {
  memcpy(dest->litlens, source->litlens,
         ZOPFLI_NUM_LL * sizeof(dest->litlens[0]));
  memcpy(dest->dists, source->dists,
         ZOPFLI_NUM_D * sizeof(dest->dists[0]));

  memcpy(dest->ll_symbols, source->ll_symbols,
         ZOPFLI_NUM_LL * sizeof(dest->ll_symbols[0]));
  memcpy(dest->d_symbols, source->d_symbols,
         ZOPFLI_NUM_D * sizeof(dest->d_symbols[0]));
}

void FreeStats(SymbolStats* stats) {
  free(stats->litlens);
  free(stats->dists);
  free(stats->ll_symbols);
  free(stats->d_symbols);
}

/* Adds the bit lengths. */
static void AddWeighedStatFreqs(const SymbolStats* stats1, zfloat w1,
                                const SymbolStats* stats2, zfloat w2,
                                SymbolStats* result) {
  size_t i;
  for (i = 0; i < ZOPFLI_NUM_LL; i++) {
    result->litlens[i] =
        (size_t) (stats1->litlens[i] * w1 + stats2->litlens[i] * w2);
  }
  for (i = 0; i < ZOPFLI_NUM_D; i++) {
    result->dists[i] =
        (size_t) (stats1->dists[i] * w1 + stats2->dists[i] * w2);
  }
  result->litlens[256] = 1;  /* End symbol. */
}

typedef struct RanState {
  unsigned int m_w, m_z;
  uint32_t Q[4096], c;
  int cmwc;
  int ranmod;
} RanState;

static void InitRanState(RanState* state, unsigned long m_wz, int cmwc, int ranmod) {
  if(cmwc) {
    const unsigned long phi = 0x9e3779b9;
    uint32_t x = (m_wz >> 16) + (m_wz & 65535);
    int i;

    state->Q[0] = x;
    state->Q[1] = x + phi;
    state->Q[2] = x + phi + phi;

    for (i = 3; i < 4096; i++)
      state->Q[i] = state->Q[i - 3] ^ state->Q[i - 2] ^ phi ^ i;

    state->c = 362436;
  }
  state->m_w = (m_wz >> 16);
  state->m_z = (m_wz & 65535);
  state->cmwc = cmwc;
  state->ranmod = ranmod;
}

/*
Use one of G. Marsaglia's random number generators:
- "Multiply-With-Carry" or,
- "Complementary-Multiply-With-Carry".
We can feed the generator with 2 unsigned shorts passed
with --rw and --rz switches.
*/
static unsigned int Ran(RanState* state) {
  if(state->cmwc) {
    uint64_t t, a=(uint64_t)18782;
    static uint32_t i=4095;
    uint32_t x,r=0xfffffffe;
    i=(i+1)&4095;
    t=a*state->Q[i]+state->c;
    state->c=(t>>32);
    x=t+state->c;
    if(x<state->c) {
      ++x;
      ++state->c;
    }
    return(state->Q[i]=r-x);
  } else {
    state->m_z = 36969 * (state->m_z & 65535) + (state->m_z >> 16);
    state->m_w = 18000 * (state->m_w & 65535) + (state->m_w >> 16);
    return (state->m_z << 16) + state->m_w;  /* 32-bit result. */
  }
}

static void RandomizeFreqs(RanState* state, size_t* freqs, unsigned n) {
  unsigned i;
  for (i = 0; i < n; i++) {
    if ((Ran(state) >> 4) % state->ranmod == 0) freqs[i] = freqs[Ran(state) % n];
  }
}

static void RandomizeStatFreqs(RanState* state, SymbolStats* stats) {
  RandomizeFreqs(state, stats->litlens, ZOPFLI_NUM_LL);
  RandomizeFreqs(state, stats->dists, ZOPFLI_NUM_D);
  stats->litlens[256] = 1;  /* End symbol. */
}

static void ClearStatFreqs(SymbolStats* stats) {
  memset(stats->litlens, 0, sizeof(*stats->litlens) * ZOPFLI_NUM_LL);
  memset(stats->dists, 0, sizeof(*stats->dists) * ZOPFLI_NUM_D);
}

/*
Function that calculates a cost based on a model for the given LZ77 symbol.
litlen: means literal symbol if dist is 0, length otherwise.
*/
typedef zfloat CostModelFun(unsigned litlen, unsigned dist, void* context);

/*
Cost model which should exactly match fixed tree.
type: CostModelFun
*/
static zfloat GetCostFixed(unsigned litlen, unsigned dist, void* unused) {
  (void)unused;
  if (dist == 0) {
    if (litlen <= 143) return 8;
    else return 9;
  } else {
    int dbits = ZopfliGetDistExtraBits(dist);
    int lbits = ZopfliGetLengthExtraBits(litlen);
    int lsym = ZopfliGetLengthSymbol(litlen);
    zfloat cost = 0;
    if (lsym <= 279) cost += 7;
    else cost += 8;
    cost += 5;  /* Every dist symbol has length 5. */
    return cost + dbits + lbits;
  }
}

/*
Cost model based on symbol statistics.
type: CostModelFun
*/
static zfloat GetCostStat(unsigned litlen, unsigned dist, void* context) {
  SymbolStats* stats = (SymbolStats*)context;
  if (dist == 0) {
    return stats->ll_symbols[litlen];
  } else {
    int lsym = ZopfliGetLengthSymbol(litlen);
    int lbits = ZopfliGetLengthExtraBits(litlen);
    int dsym = ZopfliGetDistSymbol(dist);
    int dbits = ZopfliGetDistExtraBits(dist);
    return stats->ll_symbols[lsym] + lbits + stats->d_symbols[dsym] + dbits;
  }
}

/*
Finds the minimum possible cost this cost model can return for valid length and
distance symbols.
*/
static zfloat GetCostModelMinCost(CostModelFun* costmodel, void* costcontext) {
  zfloat mincost;
  int bestlength = 0; /* length that has lowest cost in the cost model */
  int bestdist = 0; /* distance that has lowest cost in the cost model */
  int i;
  /*
  Table of distances that have a different distance symbol in the deflate
  specification. Each value is the first distance that has a new symbol. Only
  different symbols affect the cost model so only these need to be checked.
  See RFC 1951 section 3.2.5. Compressed blocks (length and distance codes).
  */
  static const int dsymbols[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513,
    769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
  };

  mincost = ZOPFLI_LARGE_FLOAT;
  for (i = 3; i < 259; i++) {
    zfloat c = costmodel(i, 1, costcontext);
    if (c < mincost) {
      bestlength = i;
      mincost = c;
    }
  }

  mincost = ZOPFLI_LARGE_FLOAT;
  for (i = 0; i < 30; i++) {
    zfloat c = costmodel(3, dsymbols[i], costcontext);
    if (c < mincost) {
      bestdist = dsymbols[i];
      mincost = c;
    }
  }

  return costmodel(bestlength, bestdist, costcontext);
}

static size_t zopfli_min(size_t a, size_t b) {
  return a < b ? a : b;
}

/*
Performs the forward pass for "squeeze". Gets the most optimal length to reach
every byte from a previous byte, using cost calculations.
s: the ZopfliBlockState
in: the input data array
instart: where to start
inend: where to stop (not inclusive)
costmodel: function to calculate the cost of some lit/len/dist pair.
costcontext: abstract context for the costmodel function
length_array: output array of size (inend - instart) which will receive the best
    length to reach this byte from a previous byte.
returns the cost that was, according to the costmodel, needed to get to the end.
*/
#ifdef NDEBUG
static void GetBestLengths(ZopfliBlockState *s,
#else
static zfloat GetBestLengths(ZopfliBlockState *s,
#endif
                             const unsigned char* in,
                             size_t instart, size_t inend,
                             CostModelFun* costmodel, void* costcontext,
                             unsigned short* length_array,
                             ZopfliHash *h, zfloat *costs) {
  /* Best cost to get here so far. */
  size_t blocksize = inend - instart;
  size_t i = 0, k, kend;
  unsigned short leng;
  unsigned short dist;
  unsigned short sublen[259];
  size_t windowstart = instart > ZOPFLI_WINDOW_SIZE
      ? instart - ZOPFLI_WINDOW_SIZE : 0;
#ifndef NDEBUG
  zfloat result;
#endif
  zfloat mincostsum;
  zfloat mincost = GetCostModelMinCost(costmodel, costcontext);

#ifndef NDEBUG
  if (instart == inend) return 0;
#endif

  ZopfliInitHash(ZOPFLI_WINDOW_SIZE, h);
  ZopfliWarmupHash(in, windowstart, inend, h);
  for (i = windowstart; i < instart; i++) {
    ZopfliUpdateHash(in, i, inend, h);
  }

  costs[0] = 0;  /* Because it's the start. */

#ifdef NDOUBLE
  memset(costs + 1, 127, blocksize * sizeof(*costs));
#else
 #ifdef LDOUBLE
  for (i = 1; i < blocksize + 1; i++) costs[i] = ZOPFLI_LARGE_FLOAT;
 #else
  memset(costs + 1, 69, blocksize * sizeof(*costs));
 #endif
#endif

  length_array[0] = 0;

  for (i = instart; i < inend; i++) {
    size_t j = i - instart;  /* Index in the costs array and length_array. */
    ZopfliUpdateHash(in, i, inend, h);

#ifdef ZOPFLI_SHORTCUT_LONG_REPETITIONS
    /* If we're in a long repetition of the same character and have more than
    ZOPFLI_MAX_MATCH characters before and after our position. */
    if (h->same[i & ZOPFLI_WINDOW_MASK] > ZOPFLI_MAX_MATCH * 2
        && i > instart + ZOPFLI_MAX_MATCH + 1
        && i + ZOPFLI_MAX_MATCH * 2 + 1 < inend
        && h->same[(i - ZOPFLI_MAX_MATCH) & ZOPFLI_WINDOW_MASK]
            > ZOPFLI_MAX_MATCH) {
      zfloat symbolcost = costmodel(ZOPFLI_MAX_MATCH, 1, costcontext);
      /* Set the length to reach each one to ZOPFLI_MAX_MATCH, and the cost to
      the cost corresponding to that length. Doing this, we skip
      ZOPFLI_MAX_MATCH values to avoid calling ZopfliFindLongestMatch. */
      for (k = 0; k < ZOPFLI_MAX_MATCH; k++) {
        costs[j + ZOPFLI_MAX_MATCH] = costs[j] + symbolcost;
        length_array[j + ZOPFLI_MAX_MATCH] = ZOPFLI_MAX_MATCH;
        i++;
        j++;
        ZopfliUpdateHash(in, i, inend, h);
      }
    }
#endif

    ZopfliFindLongestMatch(s, h, in, i, inend, ZOPFLI_MAX_MATCH, sublen,
                           &dist, &leng);

    /* Literal. */
    if (i + 1 <= inend) {
      zfloat newCost = costs[j] + costmodel(in[i], 0, costcontext);
      assert(newCost >= 0);
      if (newCost < costs[j + 1]) {
        costs[j + 1] = newCost;
        length_array[j + 1] = 1;
      }
    }
    /* Lengths. */
    kend = zopfli_min(leng, inend-i);
    mincostsum = mincost + costs[j];
    for (k = 3; k <= kend; k++) {
      zfloat newCost;

      /* Calling the cost model is expensive, avoid this if we are already at
      the minimum possible cost that it can return. */
      if (costs[j + k] <= mincostsum) continue; 

      newCost = costs[j] + costmodel(k, sublen[k], costcontext);
      assert(newCost >= 0);
      if (newCost < costs[j + k]) {
        assert(k <= ZOPFLI_MAX_MATCH);
        costs[j + k] = newCost;
        length_array[j + k] = k;
      }
    }
  }

#ifndef NDEBUG
  assert(costs[blocksize] >= 0);
  result = costs[blocksize];

  return result;
#endif
}

/*
Calculates the optimal path of lz77 lengths to use, from the calculated
length_array. The length_array must contain the optimal length to reach that
byte. The path will be filled with the lengths to use, so its data size will be
the amount of lz77 symbols.
*/
static void TraceBackwards(size_t size, const unsigned short* length_array,
                           unsigned short** path, size_t* pathsize, size_t* pathsizebuff) {
  size_t index = size;
  if (size == 0) return;

  if(*pathsizebuff == 0) {
    size_t cntr = 0;
    do {
      ++cntr;
    } while(index -= length_array[index]);
    *pathsizebuff = cntr + ZOPFLI_REALLOC_BUFFER;
    *path = Zrealloc(*path,*pathsizebuff * sizeof(*path));
    index = size;
  }

  do {
    if(*pathsize == *pathsizebuff) {
      *pathsizebuff += ZOPFLI_REALLOC_BUFFER;
      *path = Zrealloc(*path,*pathsizebuff * sizeof(*path));
    }
    (*path)[(*pathsize)++] = length_array[index];
    assert(length_array[index] <= index);
    assert(length_array[index] <= ZOPFLI_MAX_MATCH);
    assert(length_array[index] != 0);
  } while(index -= length_array[index]);

  /* Mirror result. */
  index = *pathsize >> 1;
  while(index--) {
    unsigned short temp = (*path)[index];
    (*path)[index] = (*path)[*pathsize - index - 1];
    (*path)[*pathsize - index - 1] = temp;
  }
}

static void FollowPath(ZopfliBlockState* s,
                       const unsigned char* in, size_t instart, size_t inend,
                       unsigned short* path, size_t pathsize,
                       ZopfliLZ77Store* store, ZopfliHash *h) {
  size_t i, j, pos = 0;
  size_t windowstart = instart > ZOPFLI_WINDOW_SIZE
      ? instart - ZOPFLI_WINDOW_SIZE : 0;

  size_t total_length_test = 0;

  if (instart == inend) return;

  ZopfliInitHash(ZOPFLI_WINDOW_SIZE, h);
  ZopfliWarmupHash(in, windowstart, inend, h);
  for (i = windowstart; i < instart; i++) {
    ZopfliUpdateHash(in, i, inend, h);
  }

  pos = instart;
  for (i = 0; i < pathsize; i++) {
    unsigned short length = path[i];
    unsigned short dummy_length;
    unsigned short dist;
    assert(pos < inend);

    ZopfliUpdateHash(in, pos, inend, h);

    /* Add to output. */
    if (length >= ZOPFLI_MIN_MATCH) {
      /* Get the distance by recalculating longest match. The found length
      should match the length from the path. */
      ZopfliFindLongestMatch(s, h, in, pos, inend, length, 0,
                             &dist, &dummy_length);
      assert(!(dummy_length != length && length > 2 && dummy_length > 2));
#ifndef NDEBUG
      ZopfliVerifyLenDist(in, inend, pos, dist, length);
#endif
      ZopfliStoreLitLenDist(length, dist, pos, store);
      total_length_test += length;
    } else {
      length = 1;
      ZopfliStoreLitLenDist(in[pos], 0, pos, store);
      total_length_test++;
    }

    assert(pos + length <= inend);
    for (j = 1; j < length; j++) {
      ZopfliUpdateHash(in, pos + j, inend, h);
    }

    pos += length;
  }
}

/* Calculates the entropy of the statistics */
static void CalculateStatistics(SymbolStats* stats) {
  ZopfliCalculateEntropy(stats->litlens, ZOPFLI_NUM_LL, stats->ll_symbols);
  ZopfliCalculateEntropy(stats->dists, ZOPFLI_NUM_D, stats->d_symbols);
}

/* Appends the symbol statistics from the store. */
static void GetStatistics(const ZopfliLZ77Store* store, SymbolStats* stats) {
  size_t i = store->size;
  while(i--) {
    if (store->dists[i] == 0) {
      stats->litlens[store->litlens[i]]++;
    } else {
      stats->litlens[ZopfliGetLengthSymbol(store->litlens[i])]++;
      stats->dists[ZopfliGetDistSymbol(store->dists[i])]++;
    }
  }
  stats->litlens[256] = 1;  /* End symbol. */

  CalculateStatistics(stats);
}

/*
Does a single run for ZopfliLZ77Optimal. For good compression, repeated runs
with updated statistics should be performed.
s: the block state
in: the input data array
instart: where to start
inend: where to stop (not inclusive)
path: pointer to dynamically allocated memory to store the path
pathsize: pointer to the size of the dynamic path array
length_array: array of size (inend - instart) used to store lengths
costmodel: function to use as the cost model for this squeeze run
costcontext: abstract context for the costmodel function
store: place to output the LZ77 data
returns the cost that was, according to the costmodel, needed to get to the end.
    This is not the actual cost.
*/
static void LZ77OptimalRun(ZopfliBlockState* s,
    const unsigned char* in, size_t instart, size_t inend,
    unsigned short** path, size_t* pathsize, size_t* pathsizebuff,
    unsigned short* length_array, CostModelFun* costmodel,
    void* costcontext, ZopfliLZ77Store* store,
    ZopfliHash* h, zfloat *costs) {
#ifndef NDEBUG
  zfloat cost = 
#endif
  GetBestLengths(
      s, in, instart, inend, costmodel, costcontext, length_array, h, costs);
  *pathsize = 0;
  TraceBackwards(inend - instart, length_array, path, pathsize, pathsizebuff);
  FollowPath(s, in, instart, inend, *path, *pathsize, store, h);
  assert(cost < ZOPFLI_LARGE_FLOAT);
}

/*
Here we allow user to define how many unsuccessful in size reduction
iterations need to pass to give up in finding best parameters.
(--mui switch).
*/
zfloat ZopfliLZ77Optimal(ZopfliBlockState *s,
                         const unsigned char* in, size_t instart, size_t inend,
                         ZopfliLZ77Store* store, ZopfliIterations* iterations,
                         SymbolStats** foundbest, unsigned int* startiteration) {
  /* Dist to get to here with smallest cost. */
  size_t blocksize = inend - instart;
  unsigned short* length_array = Zmalloc(sizeof(*length_array) * (blocksize + 1));
  unsigned short* path = 0;
  size_t pathsize = 0;
  size_t pathsizebuff = 0;
  ZopfliLZ77Store currentstore;
  SymbolStats stats, beststats, laststats;
  unsigned int i = *startiteration, j;
  unsigned int fails = 0, lastrandomstep = 0;
  zfloat cost;
  zfloat *costs = Zmalloc(sizeof(*costs) * (blocksize + 1));
  zfloat bestcost = ZOPFLI_LARGE_FLOAT;
  zfloat lastcost = 0;
  zfloat statsimp = (zfloat)s->options->statimportance/(zfloat)100;
  zfloat laststatsimp = 1.5 - statsimp;
  /* Try randomizing the costs a bit once the size stabilizes. */
  RanState ran_state;
  ZopfliHash hash;
  ZopfliHash* h = &hash;

  InitRanState(&ran_state, s->options->ranstatewz,
               (s->options->mode & 0x0020), s->options->ranstatemod);

  InitStats(&stats);
  InitStats(&laststats);
  InitStats(&beststats);
  ZopfliInitLZ77Store(in, &currentstore);
  ZopfliMallocHash(ZOPFLI_WINDOW_SIZE, h);

  /* Do regular deflate, then loop multiple shortest path runs, each time using
  the statistics of the previous run. */


  /* Check if we have best stats passed for this block, if we do,
     skip initial run and reduce iterations to 1 to produce best
     result right away. */

  j = s->options->numiterations;
  if(j == 0) j = (unsigned int)-2;
  if(j >= i) {
    j -= i;
  } else {
    j = 1;
  }
  ++j;
  if(j < 2) j = 2;

  if(foundbest!=NULL && *foundbest!=NULL) {
    CopyStats(*foundbest, &stats);
    if(s->options->numthreads == 0 && s->options->verbose>2)
      fprintf(stderr,"Already processed, reusing best . . .\n");
  } else {
    /* Initial run. */
    ZopfliLZ77Greedy(s, in, instart, inend, &currentstore, h);
    GetStatistics(&currentstore, &stats);
  }

  /* Repeat statistics with each time the cost model from the previous stat
  run. */
  while(--j) {
    ZopfliResetLZ77Store(&currentstore);
    LZ77OptimalRun(s, in, instart, inend, &path, &pathsize, &pathsizebuff,
                   length_array, GetCostStat, (void*)&stats,
                   &currentstore, h, costs);
    cost = ZopfliCalculateBlockSize(s->options, &currentstore, 0, currentstore.size, 2, 0);
    if(s->options->numthreads) {
      iterations->iteration = i;
      iterations->cost = (int)cost;
    } else if (s->options->verbose>4 || (s->options->verbose>2 && cost < bestcost)) {
      fprintf(stderr, "Iteration %d: %lu bit      \r", i, (unsigned long)cost);
    }
    if (cost < bestcost) {
      if(s->options->numthreads) {
        iterations->bestiteration = i;
        iterations->bestcost = (int)cost;
      } else if(!s->options->numthreads && s->options->verbose>3) {
        fprintf(stderr, "\n");
      }
      /* Start: Copy to the output store. */
      ZopfliCopyLZ77Store(&currentstore, store);
      CopyStats(&stats, &beststats);
      bestcost = cost;
      /* End */
      fails=0;
    } else {
      ++fails;
    }
    if(iterations->maxfailiterations && fails > iterations->maxfailiterations) break;
    CopyStats(&stats, &laststats);
    ClearStatFreqs(&stats);
    GetStatistics(&currentstore, &stats);
    if (i > 5 && cost == lastcost) {
      CopyStats(&beststats, &stats);
      RandomizeStatFreqs(&ran_state, &stats);
      CalculateStatistics(&stats);
      lastrandomstep = 1;
    } else if (lastrandomstep) {
      /* This makes it converge slower but better. Do it only once the
      randomness kicks in so that if the user does few iterations, it gives a
      better result sooner. */
      AddWeighedStatFreqs(&stats, statsimp, &laststats, laststatsimp, &stats);
      CalculateStatistics(&stats);
    }
    lastcost = cost;
    ++i;
  }

  *startiteration = i;

  if(!s->options->numthreads && s->options->verbose==3) {
    fprintf(stderr, "\n");
  }

  /* If we had last parameter, foundbest, NOT NULLed then use it to return best
     stats found. */
  if(foundbest!=NULL) {
    if(*foundbest==NULL) {
      *foundbest = Zmalloc(sizeof(**foundbest));
      InitStats(*foundbest);
    }
    CopyStats(&beststats, *foundbest);
  }


  free(path);
  free(costs);
  free(length_array);
  ZopfliCleanHash(h);
  ZopfliCleanLZ77Store(&currentstore);
  FreeStats(&stats);
  FreeStats(&laststats);
  FreeStats(&beststats);

  return bestcost;
}

void ZopfliLZ77OptimalFixed(ZopfliBlockState *s,
                            const unsigned char* in,
                            size_t instart, size_t inend,
                            ZopfliLZ77Store* store)
{
  /* Dist to get to here with smallest cost. */
  size_t blocksize = inend - instart;
  unsigned short* length_array = Zmalloc(sizeof(unsigned short) * (blocksize + 1));
  unsigned short* path = 0;
  size_t pathsize = 0;
  size_t pathsizebuff = 0;
  zfloat *costs = Zmalloc(sizeof(zfloat) * (blocksize + 1));
  ZopfliHash hash;
  ZopfliHash* h = &hash;
  ZopfliMallocHash(ZOPFLI_WINDOW_SIZE, h);

  s->blockstart = instart;
  s->blockend = inend;

  /* Shortest path for fixed tree This one should give the shortest possible
  result for fixed tree, no repeated runs are needed since the tree is known. */
  LZ77OptimalRun(s, in, instart, inend, &path, &pathsize, &pathsizebuff,
                 length_array, GetCostFixed, 0, store, h, costs);

  ZopfliCleanHash(h);
  free(path);
  free(costs);
  free(length_array);
}
