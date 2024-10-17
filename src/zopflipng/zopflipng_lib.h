/*
Copyright 2016 Google Inc. All Rights Reserved.
Copyright 2016 Frédéric Kayser. All Rights Reserved.
Copyright 2016 Aaron Kaluszka. All Rights Reserved.
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

Library to recompress and optimize PNG images. Uses Zopfli as the compression
backend, chooses optimal PNG color model, and tries out several PNG filter
strategies.
*/

#ifndef ZOPFLIPNG_LIB_H_
#define ZOPFLIPNG_LIB_H_

#ifdef __cplusplus

#include "../zopfli/defines.h"
#include <string>
#include <vector>
#include <algorithm>

extern "C" {

#endif

#include <stdlib.h>

enum ZopfliPNGFilterStrategy {
  kStrategyZero = 0,
  kStrategyOne = 1,
  kStrategyTwo = 2,
  kStrategyThree = 3,
  kStrategyFour = 4,
  kStrategyMinSum,
  kStrategyDistinctBytes,
  kStrategyDistinctBigrams,
  kStrategyEntropy,
  kStrategyBruteForce,
  kStrategyIncremental,
  kStrategyPredefined,
  kStrategyGeneticAlgorithm,
  kNumFilterStrategies, /* Not a strategy but used for the size of this enum */
  kStrategyNA
};

enum ZopfliPNGPalettePriority {
  kPriorityPopularity,
  kPriorityRGB,
  kPriorityYUV,
  kPriorityLab,
  kPriorityMSB,
  kNumPalettePriorities,
  kPriorityNA
};

enum ZopfliPNGPaletteDirection {
  kDirectionAscending,
  kDirectionDescending,
  kNumPaletteDirections,
  kDirectionNA
};

enum ZopfliPNGPaletteTransparency {
  kTransparencyIgnore,
  kTransparencySort,
  kTransparencyFirst,
  kNumPaletteTransparencies,
  kTransparencyNA
};

enum ZopfliPNGPaletteOrder {
  kOrderNone,
  kOrderGlobal,
  kOrderNearest,
  kOrderWeight,
  kOrderNeighbor,
  kNumPaletteOrders
};

typedef struct CZopfliPNGOptions {
  int lossy_transparent;
  int lossy_8bit;

  enum ZopfliPNGFilterStrategy* filter_strategies;
  // How many strategies to try.
  int num_filter_strategies;

  enum ZopfliPNGPalettePriority* palette_priorities;
  int num_palette_priorities;

  enum ZopfliPNGPaletteDirection* palette_directions;
  int num_palette_directions;

  enum ZopfliPNGPaletteTransparency* palette_transparencies;
  int num_palette_transparencies;

  enum ZopfliPNGPaletteOrder* palette_orders;
  int num_palette_orders;

  int auto_filter_strategy;

  char** keepchunks;
  // How many entries in keepchunks.
  int num_keepchunks;

  int use_zopfli;

  unsigned int num_iterations;

  unsigned int num_iterations_large;

  int block_split_strategy;

  int blocksplittingmax;

  int lengthscoremax;

  int verbose;

  unsigned int maxfailiterations;

  unsigned int findminimumrec;

  unsigned long ranstatewz;

  int ranstatemod;

  int pass;

  unsigned long mode;

  unsigned numthreads;
  
  unsigned numthreadsstop;

  int statimportance;

  size_t* threadaffinity;
  size_t affamount;

  size_t smallestblock;

  size_t testrecmui;

  size_t slowdynmui;

  int try_paletteless_size;

  int ga_population_size;

  int ga_max_evaluations;

  int ga_stagnate_evaluations;

  float ga_mutation_probability;

  float ga_crossover_probability;
  
  unsigned ga_tournament_size;

  int ga_number_of_offspring;

} CZopfliPNGOptions;

// Sets the default options
// Does not allocate or set keepchunks or filter_strategies
void CZopfliPNGSetDefaults(CZopfliPNGOptions *png_options);

// Returns 0 on success, error code otherwise
// The caller must free resultpng after use
int CZopfliPNGOptimize(const unsigned char* origpng,
    const size_t origpng_size,
    const CZopfliPNGOptions* png_options,
    int verbose,
    unsigned char** resultpng,
    size_t* resultpng_size);

#ifdef __cplusplus
}  // extern "C"
#endif

// C++ API
#ifdef __cplusplus

struct ZopfliPNGOptions {
  ZopfliPNGOptions();

  // Allow altering hidden colors of fully transparent pixels
  int lossy_transparent;

  // Convert 16-bit per channel images to 8-bit per channel
  bool lossy_8bit;

  // Filter strategies to try
  std::vector<ZopfliPNGFilterStrategy> filter_strategies;

  // Palette priority strategies to try
  std::vector<ZopfliPNGPalettePriority> palette_priorities;

  // Palette sort directions to try
  std::vector<ZopfliPNGPaletteDirection> palette_directions;

  // Palette transparency strategies to try
  std::vector<ZopfliPNGPaletteTransparency> palette_transparencies;

  // Palette ordering strategies to try
  std::vector<ZopfliPNGPaletteOrder> palette_orders;

  // Automatically choose filter strategy using less good compression
  bool auto_filter_strategy;
  
  // Keep original color type (RGB, RGBA, gray, gray+alpha or palette) and bit
  // depth of the PNG.
  // This results in a loss of compression opportunities, e.g. it will no
  // longer convert a 4-channel RGBA image to 2-channel gray+alpha if the image
  // only had translucent gray pixels.
  // May be useful if a device does not support decoding PNGs of a particular
  // color type.
  // Default value: false.
  bool keep_colortype;

  // PNG chunks to keep
  // chunks to literally copy over from the original PNG to the resulting one
  std::vector<std::string> keepchunks;

  // Use Zopfli deflate compression
  bool use_zopfli;

  // Zopfli number of iterations
  unsigned int num_iterations;

  // Zopfli number of iterations on large images
  unsigned int num_iterations_large;

  // Unused, left for backwards compatiblity.
  int block_split_strategy;

  // Maximum amount of blocks to split into (0 for unlimited, but this can give
  // extreme results that hurt compression on some files). Default value: 15.
  int blocksplittingmax;

  // Used to alter GetLengthScore max distance, this affects block splitting
  // model and the chance for first run being closer to the optimal output.
  int lengthscoremax;

  // Verbosity level, shared with Zopfli
  int verbose;

  /*
  Used to stop working on a block if there is specified amount of iterations
  without further bit reductions. Number of iterations should be greater
  than this value, otherwise it will have no effect.
  */
  unsigned int maxfailiterations;

  /*
  This has an impact on block splitting model by recursively checking multiple
  split points. Higher values slow down block splitting. Default is 9.
  */
  unsigned int findminimumrec;

  /*
  Initial randomness for iterations.
  Changing the default 1 and 2 allows zopfli to act more random
  on each run. W using upper 16 bits, Z lower 16 bits.
  */
  unsigned long ranstatewz;

  /*
  Modulo used by random function. By default modulo 3 is used.
  Sometimes using different values (like 5) may give better results.
  */
  int ranstatemod;

  /*
  Recompress the file this many times after splitting last, it will
  run this many times ONLY if last block splitting is still smaller.
  */
  int pass;

  /*
  KrzYmod's "DIP SWITCH":
  0x0001 - LAZY MATCHING,
  0x0002 - OPTIMIZE HUFFMAN HEADERS,
  0x0004 - REVERSE COUNTS (GCC 5.3 unstable qsort emulation),
  0x0008 - BROTLI RLE ENCODING,
  0x0010 - RUN 16 TRIES OF THE ABOVE,
  0x0020 - Use Complementary-Multiply-With-Carry,
  0x0040 - Disable splitting after compression,
  0x0080 - Use expensive fixed block calculations in splitter,
  0x0100 - Use File-based best stats DB.
  0x0200 - Use max recursion per data --bsr is the bytes limit then,
  0x0400 - Test recursion of 2 - 128 before compression.
  */
  unsigned long mode;

  /*
  Iterate multiple dynamic blocks at once using pthreads, aka.
  multi-threading mode. Passing 0 forces compatibility behavior
  by running Block processing function with MASTER thread and
  displaying old fashioned statistics.
  */
  unsigned numthreads;

  /*
  Stop compression early if there is this many or less working
  threads left. Avoids inefficient long compression times
  at the cost of certain blocks having less iterations counter.
  */
  unsigned numthreadsstop;

  /*
  Current stats to last stats importance in weighted statistic
  calculations. Default is 100, meaning 1 : 0.5.
  */
  int statimportance;

  /*
  Thread affinity which may help with schedulers that don't properly
  support separate CPU dies like Ryzen CCX.
  Defined as <number>,<number>,<number>...
  A numer specifies which cores to use per thread. For example
  to use core0+1,core3+4, core2+5+6: 3,24,100
  */
  size_t* threadaffinity;
  size_t affamount;

  /*
  Sets a minimum data size in bytes under which faster recursive
  search in splitter is replaced by expensive byte by byte
  analysis. Default is 1024 as per original.
  */
  size_t smallestblock;

  /*
  How many unsuccessful iterations to use for LZ77Optimal in splitter
  which will only be used if that number is greater than 0 in
  --testrec command.
  */
  size_t testrecmui;

  /*
  Use Optimal LZ77 in splitter which is the normal iteration mode
  trying on split points before. If > 0 use this many maximum
  unsuccessful iterations for every split point tried.
  */
  size_t slowdynmui;

  // Maximum size after which to try full color image compression on paletted image
  int try_paletteless_size;

  // Genetic algorithm: number of genomes in pool
  int ga_population_size;

  // Genetic algorithm: overall maximum number of evaluations
  int ga_max_evaluations;

  // Genetic algorithm: number of sequential evaluations without improvement
  int ga_stagnate_evaluations;

  // Genetic algorithm: probability of mutation per gene per generation
  float ga_mutation_probability;

  // Genetic algorithm: probability of crossover per generation
  float ga_crossover_probability;
  
  // Genetic algorithm: the size of tournament
  unsigned ga_tournament_size;

  // Genetic algorithm: number of offspring per generation
  int ga_number_of_offspring;

};

// Returns 0 on success, error code otherwise.
// If verbose is true, it will print some info while working.
int ZopfliPNGOptimize(const std::vector<unsigned char>& origpng,
    const ZopfliPNGOptions& png_options,
    int verbose,
    std::vector<unsigned char>* resultpng);

#endif  // __cplusplus

#endif  // ZOPFLIPNG_LIB_H_
